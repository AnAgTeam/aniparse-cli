/*
 * Copyright (C) 2025-2026 Toilettrauma
 *
 * The download verb and its dumping machinery: fetch an image container's items or a
 * manga's chapter pages and write them to disk, up to --jobs at a time, cancellable
 * via SIGINT. Also handles the stdin mode that consumes search/latest --json records
 * (from_serialized). Split out of main.cpp — the largest single block.
 */
#include "CliCommon.hpp"

#include <aniparse/Client.hpp>
#include <aniparse/net/CancellingTask.hpp> // asyncnet::NetworkTask (backend-neutral)

#include <boost/json.hpp>
#include <coro/sync_wait.hpp>
#include <coro/when_all.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio> // std::println(FILE*, …) overloads (stderr); pulled in for the FILE* form
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <print>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace anip::cli {
using namespace aniparse;

namespace {

// Diagnostic: sleep this long before each image fetch to probe source rate limits
// (some CDNs 403 a burst of page requests). 0 = off; set via download's --delay <ms>.
// Neutralized when --jobs > 1 (blocking sleep would stall the requestor thread).
unsigned g_delay_ms = 0;

// Set for the duration of a download so Ctrl-C (SIGINT) requests cancellation of
// every in-flight fetch — the shared stop_source reaches each request — instead of
// hard-killing mid-write. request_stop is thread-safe; on Windows the handler runs
// on its own thread, so touching the atomic pointer from there is fine.
std::atomic<std::stop_source*> g_active_stop{ nullptr };
void on_interrupt(int) {
	if (std::stop_source* stop = g_active_stop.load()) {
		stop->request_stop();
	}
}

/// Installs on_interrupt for SIGINT while alive, pointing it at @p stop; restores
/// the default handler on scope exit (download has many return paths).
struct SignalGuard {
	explicit SignalGuard(std::stop_source& stop) {
		g_active_stop.store(&stop);
		std::signal(SIGINT, on_interrupt);
	}
	~SignalGuard() {
		std::signal(SIGINT, SIG_DFL);
		g_active_stop.store(nullptr);
	}
	SignalGuard(const SignalGuard&) = delete;
	SignalGuard& operator=(const SignalGuard&) = delete;
};

/// A filename for a downloaded image: the URL's basename (query stripped), or a
/// synthetic item_<index> when the URL carries none.
std::string filename_from_url(std::string_view url, pageoff index) {
	const std::string_view path = url.substr(0, url.find_first_of("?#"));
	const auto slash = path.find_last_of('/');
	const std::string_view base = (slash == std::string_view::npos) ? path : path.substr(slash + 1);
	if (base.empty()) {
		return "item_" + std::to_string(static_cast<long long>(index));
	}
	return std::string(base);
}

/// Build a path from UTF-8 bytes. On Windows a path made from a narrow std::string
/// is decoded with the ANSI code page, so source-derived text (JSON titles, URL
/// basenames — all UTF-8) lands on disk as mojibake even though the console (UTF-8)
/// round-trips it back and shows it fine. Constructing from char8_t forces the
/// correct UTF-8 -> native (UTF-16) conversion. On POSIX the native encoding is
/// already UTF-8, so this is a plain copy.
std::filesystem::path utf8_path(std::string_view s) {
	return std::filesystem::path(
	    std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

/// Render a path as UTF-8 for display/JSON. The inverse of utf8_path: path::string()
/// on Windows narrows via the ANSI code page (lossy for non-Latin), so print the
/// UTF-8 form explicitly so a UTF-8 console (and JSON output) shows the real name.
std::string path_utf8(const std::filesystem::path& p) {
	const std::u8string u8 = p.u8string();
	return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

struct DumpResult {
	int ok = 0;
	int failed = 0;
	boost::json::array written;
};

/// Fetch one image and write it into @p dir. A NetworkTask (not a plain function)
/// so it composes under for_each_concurrent: co_awaited inside a worker it inherits
/// the worker's stop_source, so a cancel reaches the in-flight request. The shared
/// leaf of every download path; whole image buffered — fine for stills, video/huge
/// waits on streaming. Resumes on the requestor thread, so the counter/output writes
/// below are serialized with every other worker (one requestor thread) — no locks.
asyncnet::NetworkTask<void> fetch_and_write(RequestorContext& ctx, Image image,
                                            std::filesystem::path dir, pageoff index,
                                            bool json, DumpResult& acc) {
	if (g_delay_ms != 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(g_delay_ms));
	}
	auto resp = co_await ctx.request(GetRequest{ .url = image.url, .headers = image.headers });
	if (!resp || resp->status_code >= 400 || resp->body.empty()) {
		++acc.failed;
		std::println(stderr, "{}: item {} failed{}", program, static_cast<long long>(index),
		    resp ? std::format(" (http {})", resp->status_code) : std::string{});
		co_return;
	}
	const std::filesystem::path out = dir / utf8_path(filename_from_url(image.url, index));
	std::ofstream file(out, std::ios::binary);
	if (!file) {
		++acc.failed;
		std::println(stderr, "{}: cannot open {}", program, path_utf8(out));
		co_return;
	}
	file.write(resp->body.data(), static_cast<std::streamsize>(resp->body.size()));
	++acc.ok;
	if (json) {
		acc.written.emplace_back(path_utf8(out));
	} else {
		std::println("  {} ({} bytes)", path_utf8(out), resp->body.size());
	}
}

/// One worker: pull the next index off the shared cursor and run make_task(i) for it
/// until the range is drained or a stop is requested (stops starting new items; an
/// in-flight one is cancelled via the stop_source wired in by for_each_concurrent).
template<typename MakeTask>
asyncnet::NetworkTask<void> download_worker(std::shared_ptr<std::atomic<std::size_t>> cursor,
                                            std::size_t count, std::stop_token stop,
                                            MakeTask make_task) {
	for (std::size_t i = cursor->fetch_add(1); i < count; i = cursor->fetch_add(1)) {
		if (stop.stop_requested()) {
			break;
		}
		co_await make_task(i);
	}
}

/// Run make_task(i) for i in [0, count) with at most @p jobs in flight, all sharing
/// @p stop, and barrier-join. This is asyncnet::gather(stop, ...) inlined for a
/// runtime-sized set: wire the shared stop into each worker (update_stop_source, so a
/// cancel reaches its request) then coro::when_all them. A shared atomic cursor bounds
/// live coroutine frames to `jobs` (not `count`) — matters for thousand-page manga.
template<typename MakeTask>
asyncnet::NetworkTask<void> for_each_concurrent(std::stop_source stop, std::size_t count,
                                                unsigned jobs, MakeTask make_task) {
	if (count == 0) {
		co_return;
	}
	// Clamp width to [1, count] without std::min/max (windows.h defines min/max macros).
	unsigned width = jobs < 1u ? 1u : jobs;
	if (static_cast<std::size_t>(width) > count) {
		width = static_cast<unsigned>(count);
	}
	auto cursor = std::make_shared<std::atomic<std::size_t>>(0);
	std::vector<asyncnet::NetworkTask<void>> workers;
	workers.reserve(width);
	for (unsigned w = 0; w < width; ++w) {
		workers.push_back(download_worker(cursor, count, stop.get_token(), make_task));
	}
	for (auto& worker : workers) {
		worker.update_stop_source(stop); // so request_stop cancels the request, not just the loop
	}
	co_await coro::when_all(std::move(workers));
}

/// Replace characters a path segment can't hold on Windows/POSIX with '_'.
std::string sanitize_segment(std::string_view s) {
	std::string out;
	for (const char c : s) {
		const bool bad = c == '/' || c == '\\' || c == ':' || c == '*' || c == '?'
		              || c == '"' || c == '<' || c == '>' || c == '|';
		out += bad ? '_' : c;
	}
	return out;
}

/// A per-chapter subdirectory name: the source's own number when it has one,
/// else vol/chapter, else the 1-based index; the title is appended when present.
std::string chapter_dirname(const MangaChapterInfo& ch, std::size_t index) {
	std::string label;
	if (!ch.number.empty()) {
		label = "ch" + ch.number;
	} else if (ch.chapter != 0 || ch.volume != 0) {
		label = "vol" + std::to_string(ch.volume) + "_ch" + std::to_string(ch.chapter);
	} else {
		label = "chapter_" + std::to_string(index + 1);
	}
	if (!ch.name.empty()) {
		label += "_" + ch.name;
	}
	return sanitize_segment(label);
}

/// Parse a chapter range spec ("1-4,8,11", 1-based) into 0-based indices within
/// [0,count). Empty or "all" selects everything. nullopt (after printing) on a
/// malformed spec.
std::optional<std::vector<std::size_t>> parse_ranges(std::string_view spec, std::size_t count) {
	std::vector<std::size_t> out;
	if (spec.empty() || spec == "all") {
		out.resize(count);
		std::iota(out.begin(), out.end(), std::size_t{ 0 });
		return out;
	}
	std::size_t pos = 0;
	while (pos < spec.size()) {
		const std::size_t comma = spec.find(',', pos);
		const std::string_view tok =
		    spec.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
		pos = (comma == std::string_view::npos) ? spec.size() : comma + 1;
		if (tok.empty()) {
			continue;
		}
		const std::size_t dash = tok.find('-');
		if (dash == std::string_view::npos) {
			const auto n = parse_uint(tok);
			if (!n || *n < 1) {
				std::println(stderr, "{}: bad chapter range '{}'", program, tok);
				return std::nullopt;
			}
			if (static_cast<std::size_t>(*n) <= count) {
				out.push_back(static_cast<std::size_t>(*n) - 1);
			}
		} else {
			const auto lo = parse_uint(tok.substr(0, dash));
			const auto hi = parse_uint(tok.substr(dash + 1));
			if (!lo || !hi || *lo < 1 || *hi < *lo) {
				std::println(stderr, "{}: bad chapter range '{}'", program, tok);
				return std::nullopt;
			}
			for (long long i = *lo; i <= *hi && static_cast<std::size_t>(i) <= count; ++i) {
				out.push_back(static_cast<std::size_t>(i) - 1);
			}
		}
	}
	return out;
}

/// Fetch an image container's items and write each into @p dest, up to @p jobs at a
/// time (all cancellable via @p stop).
void dump_container(RequestorContext& ctx, ImageContainerGetter& container,
                    const std::string& dest, GetFilters filters, bool json, DumpResult& acc,
                    const std::stop_source& stop, unsigned jobs) {
	auto items = coro::sync_wait(container.items(ctx, filters));
	if (!items) {
		std::println(stderr, "{}: items failed: {}", program, items.error().message);
		++acc.failed;
		return;
	}
	std::error_code ec;
	std::filesystem::create_directories(dest, ec);
	auto& entries = items->results;
	const std::filesystem::path dest_path(dest); // CLI arg (ANSI on Windows) — path(dest) is correct
	auto make_task = [&ctx, &acc, dest_path, json, &entries](std::size_t i) {
		return fetch_and_write(ctx, entries[i].item.image, dest_path, entries[i].offset, json, acc);
	};
	coro::sync_wait(for_each_concurrent(stop, entries.size(), jobs, make_task));
}

/// Fetch a manga's chapters (those selected by @p chapters_spec) and write each
/// chapter's pages into a <dest>/<chapter> subdirectory.
void dump_manga(RequestorContext& ctx, MangaGetter& manga, const std::string& dest,
                std::string_view chapters_spec, bool json, DumpResult& acc,
                const std::stop_source& stop, unsigned jobs) {
	auto chapters = coro::sync_wait(manga.chapters_info(ctx, GetFilters{}));
	if (!chapters) {
		std::println(stderr, "{}: chapters failed: {}", program, chapters.error().message);
		++acc.failed;
		return;
	}
	const auto& list = chapters->results;
	const std::optional<std::vector<std::size_t>> selected = parse_ranges(chapters_spec, list.size());
	if (!selected) {
		++acc.failed;
		return;
	}

	for (const std::size_t idx : *selected) {
		if (stop.stop_requested()) { // Ctrl-C between chapters: stop before the next fetch
			break;
		}
		const MangaChapterInfo& ch = list[idx].item;
		const std::filesystem::path dir = std::filesystem::path(dest) / utf8_path(chapter_dirname(ch, idx));
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);

		auto pages = coro::sync_wait(manga.chapter_pages(ctx, ch.ref(), GetFilters{}));
		if (!pages) {
			std::println(stderr, "{}: chapter {} pages failed: {}", program, idx + 1,
			    pages.error().message);
			++acc.failed;
			continue;
		}
		auto& page_list = pages->results;
		std::println("Chapter {} ({} pages)", ch.number.empty() ? std::to_string(idx + 1) : ch.number,
		    page_list.size());
		// Pages fan out (bounded by jobs); chapters stay sequential so their headers
		// and per-chapter dirs don't interleave.
		auto make_task = [&ctx, &acc, dir, json, &page_list](std::size_t i) {
			return fetch_and_write(ctx, page_list[i].item.image, dir, page_list[i].offset, json, acc);
		};
		coro::sync_wait(for_each_concurrent(stop, page_list.size(), jobs, make_task));
	}
}

int report(DumpResult result, const std::string& dest, bool json) {
	if (json) {
		boost::json::object o;
		o["written"] = std::move(result.written);
		o["ok"]      = result.ok;
		o["failed"]  = result.failed;
		std::println("{}", boost::json::serialize(boost::json::value(std::move(o))));
	} else {
		std::println("Downloaded {} file(s) to {}{}", result.ok, dest,
		    result.failed > 0 ? std::format(" ({} failed)", result.failed) : std::string{});
	}
	return (result.ok == 0 && result.failed > 0) ? Runtime : Ok;
}

/// Rebuild a container from one piped {parser, handle} record (as emitted by
/// search/latest --json) and dump it into @p acc via from_serialized.
void dump_serialized(ParserStore& store, RequestorContext& ctx, const boost::json::object& record,
                     const std::string& dest, GetFilters filters, std::string_view chapters_spec,
                     bool json, DumpResult& acc, const std::stop_source& stop, unsigned jobs) {
	const auto* parser_field = record.if_contains("parser");
	const auto* handle_field = record.if_contains("handle");
	if (!parser_field || !parser_field->is_string() || !handle_field || !handle_field->is_object()) {
		std::println(stderr, "{}: skipping record without parser/handle", program);
		++acc.failed;
		return;
	}
	const std::string pid(parser_field->as_string().c_str());
	const std::shared_ptr<Parser> parser = store.find_by_key(pid);
	if (!parser) {
		std::println(stderr, "{}: skipping unknown parser '{}'", program, pid);
		++acc.failed;
		return;
	}
	using namespace compatibilities_flags;
	const CompatibilitiesFlags flags = parser->compatibilities().flags;
	const bool is_images = flags.has(supports_images_store) || flags.has(supports_images_search);
	const bool is_manga  = flags.has(supports_manga_store);
	if (!is_images && !is_manga) {
		std::println(stderr, "{}: skipping parser '{}' (no downloadable category)", program, pid);
		++acc.failed;
		return;
	}

	SerializedGetterData data;
	const boost::json::object& handle = handle_field->as_object();
	if (const auto* u = handle.if_contains("url"); u && u->is_string()) {
		data.url = u->as_string().c_str();
	}
	if (const auto* p = handle.if_contains("params"); p && p->is_object()) {
		for (const auto& [key, value] : p->as_object()) {
			if (value.is_string()) {
				data.params[std::string(key)] = value.as_string().c_str();
			}
		}
	}

	RequestorContext ready = ctx.new_with_config(parser->make_config(ctx.config()));
	if (is_images) {
		auto root = parser->images_getter();
		auto getter = coro::sync_wait(root->from_serialized(std::move(data)));
		if (!getter) {
			std::println(stderr, "{}: from_serialized failed for '{}': {}", program, pid, getter.error().message);
			++acc.failed;
			return;
		}
		dump_container(ready, **getter, dest, filters, json, acc, stop, jobs);
	} else {
		auto root = parser->mangas_getter();
		auto getter = coro::sync_wait(root->from_serialized(std::move(data)));
		if (!getter) {
			std::println(stderr, "{}: from_serialized failed for '{}': {}", program, pid, getter.error().message);
			++acc.failed;
			return;
		}
		dump_manga(ready, **getter, dest, chapters_spec, json, acc, stop, jobs);
	}
}

} // namespace

int download(std::span<const std::string_view> args, bool json) {
	// First positional (non-flag) token is the URL; skip the value-taking flags so
	// their arguments aren't mistaken for it.
	std::optional<std::string_view> url;
	for (std::size_t i = 0; i < args.size(); ++i) {
		const std::string_view a = args[i];
		if (a == "--dest" || a == "--limit" || a == "--chapters" || a == "--delay" || a == "--jobs") {
			++i; // consume the flag's value
			continue;
		}
		if (a.starts_with('-')) {
			continue;
		}
		url = a;
		break;
	}
	std::string dest = ".";
	if (const auto d = flag_value(args, "--dest")) {
		dest = std::string(*d);
	}
	GetFilters filters; // default: every item of the container
	if (const auto v = flag_value(args, "--limit")) {
		const auto n = parse_uint(*v);
		if (!n) {
			std::println(stderr, "{}: --limit expects a non-negative integer", program);
			return Usage;
		}
		filters.limit = static_cast<std::size_t>(*n);
	}
	std::string chapters_spec; // manga only; empty = all chapters
	if (const auto c = flag_value(args, "--chapters")) {
		chapters_spec = std::string(*c);
	}
	if (const auto d = flag_value(args, "--delay")) { // diagnostic: ms between image fetches
		const auto n = parse_uint(*d);
		if (!n) {
			std::println(stderr, "{}: --delay expects a non-negative integer (milliseconds)", program);
			return Usage;
		}
		g_delay_ms = static_cast<unsigned>(*n);
	}
	unsigned jobs = 1; // concurrent image fetches; 1 = sequential
	if (const auto j = flag_value(args, "--jobs")) {
		const auto n = parse_uint(*j);
		if (!n || *n < 1) {
			std::println(stderr, "{}: --jobs expects a positive integer", program);
			return Usage;
		}
		jobs = static_cast<unsigned>(*n);
	}
	if (jobs > 1 && g_delay_ms != 0) {
		// A blocking per-fetch sleep would stall the single requestor thread and
		// serialize the workers; jobs is the throttle here, so drop the delay.
		std::println(stderr, "{}: --delay ignored with --jobs > 1 (jobs bounds concurrency instead)", program);
		g_delay_ms = 0;
	}

	// One stop_source for the whole download; SIGINT cancels every in-flight fetch.
	std::stop_source stop;
	SignalGuard signal_guard(stop);

	ParserStore store;
	populate_store(store);
	auto client = std::make_shared<AsyncClient>();
	RequestorContext ctx(client);

	// URL mode: a pasted container / manga URL.
	if (url) {
		std::optional<UrlRoute> route = store.route_url(*url);
		if (!route) {
			std::println(stderr, "{}: no source handles '{}'", program, *url);
			return Runtime;
		}
		RequestorContext ready = ctx.new_with_config(route->parser->make_config(ctx.config()));
		DumpResult result;
		if (route->type == GetterSuggestionType::Images) {
			auto root = route->parser->images_getter();
			auto getter = coro::sync_wait(root->parse_url(ready, std::move(route->url)));
			if (!getter) {
				std::println(stderr, "{}: parse failed: {}", program, getter.error().message);
				return Runtime;
			}
			dump_container(ready, **getter, dest, filters, json, result, stop, jobs);
		} else if (route->type == GetterSuggestionType::Manga) {
			auto root = route->parser->mangas_getter();
			auto getter = coro::sync_wait(root->parse_url(ready, std::move(route->url)));
			if (!getter) {
				std::println(stderr, "{}: parse failed: {}", program, getter.error().message);
				return Runtime;
			}
			dump_manga(ready, **getter, dest, chapters_spec, json, result, stop, jobs);
		} else {
			std::println(stderr, "{}: download supports image and manga URLs", program);
			return Usage;
		}
		return report(std::move(result), dest, json);
	}

	// stdin mode: consume the JSON that `search`/`latest --json` emits — records
	// carrying {parser, handle} — and dump each via from_serialized. This is what
	// closes `anip search --json | ... | anip download`.
	std::string input((std::istreambuf_iterator<char>(std::cin)),
	                  std::istreambuf_iterator<char>());
	// Some shells (PowerShell) prepend a UTF-8 BOM when piping to a native stdin;
	// strip it so the JSON parser sees a clean '['.
	if (input.starts_with("\xEF\xBB\xBF")) {
		input.erase(0, 3);
	}
	if (input.find_first_not_of(" \t\r\n") == std::string::npos) {
		std::println(stderr, "{}: download needs a <url>, or JSON records on stdin", program);
		return Usage;
	}
	boost::json::value doc;
	try {
		doc = boost::json::parse(input);
	} catch (const std::exception& e) {
		std::println(stderr, "{}: stdin is not valid JSON ({}).", program, e.what());
		std::println(stderr, "{}: pipe search/latest with --json, e.g. "
		    "`{} --json search -p X -q foo | {} download`", program, program, program);
		return Usage;
	}

	DumpResult result;
	if (doc.is_array()) {
		for (const auto& v : doc.as_array()) {
			if (v.is_object()) {
				dump_serialized(store, ctx, v.as_object(), dest, filters, chapters_spec, json, result, stop, jobs);
			}
		}
	} else if (doc.is_object()) {
		dump_serialized(store, ctx, doc.as_object(), dest, filters, chapters_spec, json, result, stop, jobs);
	} else {
		std::println(stderr, "{}: stdin JSON must be an object or array of records", program);
		return Usage;
	}
	return report(std::move(result), dest, json);
}

} // namespace anip::cli
