/*
 * Copyright (C) 2025-2026 Toilettrauma
 *
 * anip — a command-line interface to libaniparse. Not a downloader: the point is
 * automation over the whole library (search --json | jq | parse --download,
 * cron trackers on latest --json). `list parsers`, `latest` and `parse` are
 * implemented end to end; support/search are wired in but not yet filled.
 */
#include <aniparse/ParserStore.hpp>
#include <aniparse/Client.hpp>
#include <aniparse/parsers/DefaultParsers.hpp>

#include "anip_extensions.hpp" // generated: register_extensions (seam A)

#include <boost/json.hpp>
#include <coro/sync_wait.hpp>

#include <charconv>
#include <cstdint>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace aniparse;

constexpr std::string_view program = "anip";

// 0 ok, 1 runtime error, 2 usage/validation error. --help exits 0.
enum ExitCode : int { Ok = 0, Runtime = 1, Usage = 2 };

int usage() {
	std::println(stderr,
	    "Usage: {0} [--help] [--json] <command> [<args> ...]\n"
	    "\n"
	    "Commands:\n"
	    "  list parsers            list the available sources\n"
	    "  list filters            list the library's search-filter vocabulary\n"
	    "  support (latest|search) -p <parser>   show what a source supports\n"
	    "  latest -p <parser> [--from N] [--limit N] [--sort KEY] [--asc]\n"
	    "  search -p <parser> -q <query> [--filter k=v ...] [--from N] [--limit N]\n"
	    "  parse <url>             route a URL to its source and fetch info\n"
	    "\n"
	    "Global:\n"
	    "  --json   emit machine-readable JSON instead of human text\n"
	    "  --help   show this help and exit\n",
	    program);
	return Usage;
}

/// Decode the capability bitfield into short human labels for a listing row.
std::vector<std::string_view> capability_labels(const CompatibilitiesFlags& f) {
	using namespace compatibilities_flags;
	std::vector<std::string_view> out;
	if (f.has(supports_manga_store))   out.push_back("manga");
	if (f.has(supports_anime_store))   out.push_back("anime");
	if (f.has(supports_images_store))  out.push_back("images");
	if (f.has(supports_video_store))   out.push_back("video");
	if (f.has(supports_images_search)) out.push_back("images-search");
	if (f.has(supports_registration))  out.push_back("login");
	if (f.has(supports_suggestions))   out.push_back("suggest");
	if (f.has(adult_source))           out.push_back("adult");
	return out;
}

std::string join(const std::vector<std::string_view>& parts, std::string_view sep) {
	std::string out;
	for (const std::string_view p : parts) {
		if (!out.empty()) {
			out += sep;
		}
		out += p;
	}
	return out;
}

/// Build a store: the public showcase parsers plus any extension parser sets
/// linked into this build (seam A — see cmake/anip_extensions.hpp.in).
void populate_store(ParserStore& store) {
	parsers::emplace_default_parsers(store);
	anip::register_extensions(store);
}

int list_parsers(bool json) {
	ParserStore store;
	populate_store(store);
	const std::vector<std::shared_ptr<Parser>> parser_list = store.parsers();

	if (json) {
		boost::json::array arr;
		for (const std::shared_ptr<Parser>& p : parser_list) {
			const ParserInfo info = p->info();
			boost::json::array caps;
			for (const std::string_view c : capability_labels(p->compatibilities().flags)) {
				caps.emplace_back(std::string(c));
			}
			boost::json::object o;
			o["id"]           = p->identifier();
			o["name"]         = info.name;
			o["language"]     = info.primary_language;
			o["capabilities"] = std::move(caps);
			arr.push_back(std::move(o));
		}
		std::println("{}", boost::json::serialize(boost::json::value(std::move(arr))));
		return Ok;
	}

	for (const std::shared_ptr<Parser>& p : parser_list) {
		const ParserInfo info = p->info();
		std::println("{:<12} {:<16} [{}]  {}",
		    p->identifier(), info.name, info.primary_language,
		    join(capability_labels(p->compatibilities().flags), ", "));
	}
	return Ok;
}

int list(std::span<const std::string_view> args, bool json) {
	if (args.empty()) {
		std::println(stderr, "{}: list needs a subcommand (parsers|filters)", program);
		return usage();
	}
	if (args[0] == "parsers") {
		return list_parsers(json);
	}
	if (args[0] == "filters") {
		std::println(stderr, "{}: 'list filters' is not implemented yet", program);
		return Usage;
	}
	std::println(stderr, "{}: unknown list subcommand '{}'", program, args[0]);
	return usage();
}

/// Placeholder for the networked verbs: dispatch is real, the fetch is pending.
int not_yet(std::string_view verb) {
	std::println(stderr, "{}: '{}' is not implemented yet (networked verbs are next)",
	    program, verb);
	return Usage;
}

/// The value token following @p name (e.g. "-p"), or nullopt if @p name is absent
/// or has no following token.
std::optional<std::string_view> flag_value(std::span<const std::string_view> args,
                                           std::string_view name) {
	for (std::size_t i = 0; i + 1 < args.size(); ++i) {
		if (args[i] == name) {
			return args[i + 1];
		}
	}
	return std::nullopt;
}

bool has_flag(std::span<const std::string_view> args, std::string_view name) {
	for (const std::string_view a : args) {
		if (a == name) {
			return true;
		}
	}
	return false;
}

/// Parse a non-negative integer that consumes the whole token; nullopt otherwise.
std::optional<long long> parse_uint(std::string_view s) {
	long long value = 0;
	const auto* const end = s.data() + s.size();
	const auto [ptr, ec] = std::from_chars(s.data(), end, value);
	if (ec != std::errc{} || ptr != end || value < 0) {
		return std::nullopt;
	}
	return value;
}

/// Fetch and print a page of latest containers. Templated over the root getter so
/// one body serves both manga and images: their latest() returns the same
/// PageResults<unique_ptr<...>> shape and both leaf infos expose .title.
template <typename RootGetterPtr>
int emit_latest_page(RequestorContext& ctx, RootGetterPtr root, GetFilters filters, bool json) {
	auto page = coro::sync_wait(root->latest(ctx, filters));
	if (!page) {
		std::println(stderr, "{}: latest failed: {}", program, page.error().message);
		return Runtime;
	}

	boost::json::array arr;
	int skipped = 0;
	for (auto& entry : page->results) {
		auto info = coro::sync_wait(entry.item->preview_info(ctx));
		if (!info) {
			++skipped; // a per-item preview fetch failed; keep going, report the count
			continue;
		}
		if (json) {
			boost::json::object o;
			o["offset"] = static_cast<std::int64_t>(entry.offset);
			o["title"]  = info->title;
			arr.push_back(std::move(o));
		} else {
			std::println("{:>4}  {}", static_cast<long long>(entry.offset), info->title);
		}
	}

	if (json) {
		std::println("{}", boost::json::serialize(boost::json::value(std::move(arr))));
	} else if (skipped > 0) {
		std::println(stderr, "{}: {} item(s) skipped (preview fetch failed)", program, skipped);
	}
	return Ok;
}

int latest(std::span<const std::string_view> args, bool json) {
	const std::optional<std::string_view> pkey = flag_value(args, "-p");
	if (!pkey) {
		std::println(stderr, "{}: latest needs -p <parser>", program);
		return Usage;
	}

	GetFilters filters;
	if (const auto v = flag_value(args, "--from")) {
		const auto n = parse_uint(*v);
		if (!n) {
			std::println(stderr, "{}: --from expects a non-negative integer", program);
			return Usage;
		}
		filters.from = static_cast<pageoff>(*n);
	}
	if (const auto v = flag_value(args, "--limit")) {
		const auto n = parse_uint(*v);
		if (!n) {
			std::println(stderr, "{}: --limit expects a non-negative integer", program);
			return Usage;
		}
		filters.limit = static_cast<std::size_t>(*n);
	} else {
		// A listing default: bound the per-item preview fetches rather than pulling
		// the source's whole default feed.
		filters.limit = 20;
	}
	if (const auto v = flag_value(args, "--sort")) {
		filters.sort = SortOrder{ .key = std::string(*v), .ascending = has_flag(args, "--asc") };
	}

	ParserStore store;
	populate_store(store);
	const std::shared_ptr<Parser> parser = store.find_by_key(*pkey);
	if (!parser) {
		std::println(stderr, "{}: no parser '{}' (try `{} list parsers`)", program, *pkey, program);
		return Usage;
	}

	auto client = std::make_shared<AsyncClient>();
	RequestorContext ctx(client);
	RequestorContext ready = ctx.new_with_config(parser->make_config(ctx.config()));

	using namespace compatibilities_flags;
	const CompatibilitiesFlags flags = parser->compatibilities().flags;
	if (flags.has(supports_manga_store)) {
		return emit_latest_page(ready, parser->mangas_getter(), filters, json);
	}
	if (flags.has(supports_images_store) || flags.has(supports_images_search)) {
		return emit_latest_page(ready, parser->images_getter(), filters, json);
	}
	std::println(stderr, "{}: parser '{}' has no browsable latest feed", program, *pkey);
	return Usage;
}

/// Print full info for a single parsed container. Templated over the getter so
/// one body serves manga and images — both infos expose title/description/tags/id.
template <typename GetterPtr>
int emit_parsed(RequestorContext& ctx, GetterPtr getter, std::string_view source,
                std::string_view category, bool json) {
	auto info = coro::sync_wait(getter->info(ctx));
	if (!info) {
		std::println(stderr, "{}: fetch failed: {}", program, info.error().message);
		return Runtime;
	}

	if (json) {
		boost::json::array tags;
		for (const Tag& t : info->tags) {
			tags.emplace_back(t.name);
		}
		boost::json::object o;
		o["source"]      = std::string(source);
		o["category"]    = std::string(category);
		o["id"]          = static_cast<std::int64_t>(info->id);
		o["title"]       = info->title;
		o["description"] = info->description.text;
		o["tags"]        = std::move(tags);
		std::println("{}", boost::json::serialize(boost::json::value(std::move(o))));
		return Ok;
	}

	std::println("Source:   {} ({})", source, category);
	std::println("Id:       {}", static_cast<long long>(info->id));
	std::println("Title:    {}", info->title);
	if (!info->description.text.empty()) {
		std::println("Summary:  {}", info->description.text);
	}
	if (!info->tags.empty()) {
		std::vector<std::string_view> names;
		for (const Tag& t : info->tags) {
			names.push_back(t.name);
		}
		std::println("Tags:     {}", join(names, ", "));
	}
	return Ok;
}

int parse_verb(std::span<const std::string_view> args, bool json) {
	std::optional<std::string_view> url;
	for (const std::string_view a : args) {
		if (!a.starts_with('-')) {
			url = a;
			break;
		}
	}
	if (!url) {
		std::println(stderr, "{}: parse needs a <url>", program);
		return Usage;
	}

	ParserStore store;
	populate_store(store);
	std::optional<UrlRoute> route = store.route_url(*url);
	if (!route) {
		std::println(stderr, "{}: no source handles '{}' (try `{} list parsers`)",
		    program, *url, program);
		return Runtime;
	}

	auto client = std::make_shared<AsyncClient>();
	RequestorContext ctx(client);
	RequestorContext ready = ctx.new_with_config(route->parser->make_config(ctx.config()));
	const std::string source = route->parser->info().name;

	// route->url is consumed by parse_url; keep the root getter in a named local so
	// it outlives the coroutine that reads from it.
	if (route->type == GetterSuggestionType::Manga) {
		auto root = route->parser->mangas_getter();
		auto getter = coro::sync_wait(root->parse_url(ready, std::move(route->url)));
		if (!getter) {
			std::println(stderr, "{}: parse failed: {}", program, getter.error().message);
			return Runtime;
		}
		return emit_parsed(ready, std::move(*getter), source, "manga", json);
	}
	if (route->type == GetterSuggestionType::Images) {
		auto root = route->parser->images_getter();
		auto getter = coro::sync_wait(root->parse_url(ready, std::move(route->url)));
		if (!getter) {
			std::println(stderr, "{}: parse failed: {}", program, getter.error().message);
			return Runtime;
		}
		return emit_parsed(ready, std::move(*getter), source, "images", json);
	}
	std::println(stderr, "{}: '{}' routed to {} but its category is not supported yet",
	    program, *url, source);
	return Runtime;
}

int real_main(std::span<const std::string_view> args) {
	// Split a single global flag (--json) from the verb + its arguments. --help
	// anywhere shows help and exits 0.
	bool json = false;
	std::vector<std::string_view> rest;
	for (const std::string_view a : args) {
		if (a == "--help" || a == "-h") {
			usage();
			return Ok;
		}
		if (a == "--json") {
			json = true;
			continue;
		}
		rest.push_back(a);
	}

	if (rest.empty()) {
		return usage();
	}

	const std::string_view verb = rest[0];
	const std::span<const std::string_view> verb_args(rest.begin() + 1, rest.end());

	if (verb == "list")    return list(verb_args, json);
	if (verb == "support") return not_yet("support");
	if (verb == "latest")  return latest(verb_args, json);
	if (verb == "search")  return not_yet("search");
	if (verb == "parse")   return parse_verb(verb_args, json);

	std::println(stderr, "{}: unknown command '{}'", program, verb);
	return usage();
}
} // namespace

int main(int argc, const char** argv) {
	std::vector<std::string_view> args;
	args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
	for (int i = 1; i < argc; ++i) {
		args.emplace_back(argv[i]);
	}

	try {
		return real_main(args);
	}
	catch (const std::exception& e) {
		std::println(stderr, "{}: error: {}", program, e.what());
		return Runtime;
	}
	catch (...) {
		std::println(stderr, "{}: unknown error", program);
		return Runtime;
	}
}
