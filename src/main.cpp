/*
 * Copyright (C) 2025-2026 Toilettrauma
 *
 * anip — a command-line interface to libaniparse. Not a downloader: the point is
 * automation over the whole library (search --json | jq | parse --download,
 * cron trackers on latest --json). All verbs are implemented; download handles
 * image containers by buffering each file (request()->body), with streaming/CBZ
 * and a serialized-handle input (to close search|download) still to come.
 */
#include <aniparse/ParserStore.hpp>
#include <aniparse/Client.hpp>
#include <aniparse/parsers/DefaultParsers.hpp>

#include "anip_extensions.hpp" // generated: register_extensions (seam A)

#include <boost/json.hpp>
#include <coro/sync_wait.hpp>

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
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
	    "  download [<url>] [--dest DIR] [--limit N]  save an image container's files\n"
	    "                          (no <url>: read search/latest --json records from stdin)\n"
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

int list_filters(bool json); // defined below

int list(std::span<const std::string_view> args, bool json) {
	if (args.empty()) {
		std::println(stderr, "{}: list needs a subcommand (parsers|filters)", program);
		return usage();
	}
	if (args[0] == "parsers") {
		return list_parsers(json);
	}
	if (args[0] == "filters") {
		return list_filters(json);
	}
	std::println(stderr, "{}: unknown list subcommand '{}'", program, args[0]);
	return usage();
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

/// Parse the paging/sort flags shared by latest and search (--from/--limit/--sort
/// [--asc]). nullopt (after printing) on a malformed number; --limit defaults to
/// 20 to bound the per-item preview fetches.
std::optional<GetFilters> parse_filters(std::span<const std::string_view> args) {
	GetFilters filters;
	if (const auto v = flag_value(args, "--from")) {
		const auto n = parse_uint(*v);
		if (!n) {
			std::println(stderr, "{}: --from expects a non-negative integer", program);
			return std::nullopt;
		}
		filters.from = static_cast<pageoff>(*n);
	}
	if (const auto v = flag_value(args, "--limit")) {
		const auto n = parse_uint(*v);
		if (!n) {
			std::println(stderr, "{}: --limit expects a non-negative integer", program);
			return std::nullopt;
		}
		filters.limit = static_cast<std::size_t>(*n);
	} else {
		filters.limit = 20;
	}
	if (const auto v = flag_value(args, "--sort")) {
		filters.sort = SortOrder{ .key = std::string(*v), .ascending = has_flag(args, "--asc") };
	}
	return filters;
}

/// Print a fetched page of container getters (shared by latest and search).
/// Templated over the awaited result so one body serves both manga and images:
/// latest()/search() return the same PageResults<unique_ptr<...>> and both leaf
/// infos expose .title (fetched per item via preview_info). In --json each row
/// also carries the parser id + the item's serialize() handle, so the output
/// pipes straight into `download` (which rebuilds the getter via from_serialized).
template <typename PageResult>
int emit_page(RequestorContext& ctx, PageResult page, std::string_view parser_id, bool json) {
	if (!page) {
		std::println(stderr, "{}: request failed: {}", program, page.error().message);
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
			o["parser"] = std::string(parser_id);
			// The opaque persistence handle (serialize()), not a URL — download
			// feeds it back through from_serialized. @see [[serialize-vs-parse-url]]
			if (auto handle = coro::sync_wait(entry.item->serialize())) {
				boost::json::object h;
				h["url"] = handle->url;
				boost::json::object params;
				for (const auto& [key, value] : handle->params) {
					params[key] = value;
				}
				h["params"] = std::move(params);
				o["handle"] = std::move(h);
			}
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

	const std::optional<GetFilters> filters = parse_filters(args);
	if (!filters) {
		return Usage;
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
		auto root = parser->mangas_getter();
		return emit_page(ready, coro::sync_wait(root->latest(ready, *filters)), parser->identifier(), json);
	}
	if (flags.has(supports_images_store) || flags.has(supports_images_search)) {
		auto root = parser->images_getter();
		return emit_page(ready, coro::sync_wait(root->latest(ready, *filters)), parser->identifier(), json);
	}
	std::println(stderr, "{}: parser '{}' has no browsable latest feed", program, *pkey);
	return Usage;
}

int search(std::span<const std::string_view> args, bool json) {
	const std::optional<std::string_view> pkey = flag_value(args, "-p");
	if (!pkey) {
		std::println(stderr, "{}: search needs -p <parser>", program);
		return Usage;
	}
	const std::optional<std::string_view> query = flag_value(args, "-q");
	if (!query) {
		std::println(stderr, "{}: search needs -q <query>", program);
		return Usage;
	}
	const std::optional<GetFilters> filters = parse_filters(args);
	if (!filters) {
		return Usage;
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

	// Free-text query only for now; structured --filter k=v needs the SearchItems
	// (SearchItemVariant) builder and is deferred.
	SearchRequestQuery request;
	request.query = std::string(*query);

	using namespace compatibilities_flags;
	const CompatibilitiesFlags flags = parser->compatibilities().flags;
	if (flags.has(supports_manga_store)) {
		auto root = parser->mangas_getter();
		return emit_page(ready, coro::sync_wait(root->search(ready, request, *filters)), parser->identifier(), json);
	}
	if (flags.has(supports_images_search)) {
		auto root = parser->images_getter();
		return emit_page(ready, coro::sync_wait(root->search(ready, request, *filters)), parser->identifier(), json);
	}
	std::println(stderr, "{}: parser '{}' does not support search", program, *pkey);
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

int list_filters(bool json) {
	using namespace search_keys;
	struct Key { std::string_view key; std::string_view about; };
	// The canonical filter keys the library exposes (values a source may accept and
	// that --filter will speak). episodes is an alias of pages; both map to "icount".
	static constexpr Key keys[] = {
	    { title,           "title text" },
	    { series,          "series / franchise" },
	    { tag,             "a content tag" },
	    { pages,           "chapter/page count (alias: episodes)" },
	    { status,          "publication status" },
	    { rating,          "content rating" },
	    { year,            "release year" },
	    { release_time,    "release date" },
	    { upload_time,     "upload date" },
	    { age_restriction, "minimum age" },
	    { artist,          "artist" },
	    { character,       "character" },
	    { group,           "scanlation / translation group" },
	    { type,            "media type" },
	    { language,        "language" },
	};

	if (json) {
		boost::json::array arr;
		for (const Key& k : keys) {
			boost::json::object o;
			o["key"]   = std::string(k.key);
			o["about"] = std::string(k.about);
			arr.push_back(std::move(o));
		}
		std::println("{}", boost::json::serialize(boost::json::value(std::move(arr))));
		return Ok;
	}

	std::println("Canonical search-filter keys (the library vocabulary):");
	for (const Key& k : keys) {
		std::println("  {:<10} {}", k.key, k.about);
	}
	std::println("\nWhich a source actually accepts: {} support search -p <parser>", program);
	return Ok;
}

std::string sort_directions(const SortDescriptor& d) {
	std::string out;
	if (d.ascending) {
		out += "asc";
	}
	if (d.descending) {
		if (!out.empty()) {
			out += "/";
		}
		out += "desc";
	}
	return out.empty() ? "(none)" : out;
}

int print_search_support(std::string_view source, std::string_view category,
                         const SearchCompatibilities& sc, bool json) {
	if (json) {
		boost::json::array filters;
		for (const auto& [key, value] : sc.supported_filters) {
			filters.emplace_back(key);
		}
		boost::json::array sorts;
		for (const auto& [key, dir] : sc.supported_sorts) {
			boost::json::object o;
			o["key"]  = key;
			o["asc"]  = dir.ascending;
			o["desc"] = dir.descending;
			sorts.push_back(std::move(o));
		}
		boost::json::array flags;
		for (const std::string_view f : capability_labels(sc.compatibilities)) {
			flags.emplace_back(f);
		}
		boost::json::array kinds;
		for (const std::string& k : sc.supported_suggestion_kinds) {
			kinds.emplace_back(k);
		}
		boost::json::object o;
		o["source"]           = std::string(source);
		o["category"]         = std::string(category);
		o["filters"]          = std::move(filters);
		o["sorts"]            = std::move(sorts);
		o["flags"]            = std::move(flags);
		o["suggestion_kinds"] = std::move(kinds);
		std::println("{}", boost::json::serialize(boost::json::value(std::move(o))));
		return Ok;
	}

	std::println("Source:  {} ({}) — search", source, category);
	if (sc.supported_filters.empty()) {
		std::println("Filters: (free-text query only)");
	} else {
		std::vector<std::string_view> keys;
		for (const auto& [key, value] : sc.supported_filters) {
			keys.push_back(key);
		}
		std::println("Filters: {}", join(keys, ", "));
	}
	if (sc.supported_sorts.empty()) {
		std::println("Sorts:   (source default only)");
	} else {
		std::println("Sorts:");
		for (const auto& [key, dir] : sc.supported_sorts) {
			std::println("  {:<14} [{}]", key, sort_directions(dir));
		}
	}
	std::println("Flags:   {}", join(capability_labels(sc.compatibilities), ", "));
	if (!sc.supported_suggestion_kinds.empty()) {
		std::vector<std::string_view> kinds(sc.supported_suggestion_kinds.begin(),
		                                    sc.supported_suggestion_kinds.end());
		std::println("Suggest: {}", join(kinds, ", "));
	}
	return Ok;
}

int print_latest_support(std::string_view source, std::string_view category,
                         const SupportedSorts& sorts, const CompatibilitiesFlags& flags,
                         bool json) {
	if (json) {
		boost::json::array sortarr;
		for (const auto& [key, dir] : sorts) {
			boost::json::object o;
			o["key"]  = key;
			o["asc"]  = dir.ascending;
			o["desc"] = dir.descending;
			sortarr.push_back(std::move(o));
		}
		boost::json::array flagarr;
		for (const std::string_view f : capability_labels(flags)) {
			flagarr.emplace_back(f);
		}
		boost::json::object o;
		o["source"]   = std::string(source);
		o["category"] = std::string(category);
		o["sorts"]    = std::move(sortarr);
		o["flags"]    = std::move(flagarr);
		std::println("{}", boost::json::serialize(boost::json::value(std::move(o))));
		return Ok;
	}

	std::println("Source:  {} ({}) — latest", source, category);
	if (sorts.empty()) {
		std::println("Sorts:   (source default only)");
	} else {
		std::println("Sorts:");
		for (const auto& [key, dir] : sorts) {
			std::println("  {:<14} [{}]", key, sort_directions(dir));
		}
	}
	std::println("Flags:   {}", join(capability_labels(flags), ", "));
	return Ok;
}

/// sync_wait a root getter's search_support and unwrap it; nullopt (after
/// printing) on error. Templated so one body serves manga and images roots.
template <typename RootGetterPtr>
std::optional<SearchCompatibilities> fetch_search_support(RequestorContext& ctx, RootGetterPtr root) {
	auto r = coro::sync_wait(root->search_support(ctx));
	if (!r) {
		std::println(stderr, "{}: search_support failed: {}", program, r.error().message);
		return std::nullopt;
	}
	return std::move(*r);
}

int support(std::span<const std::string_view> args, bool json) {
	if (args.empty() || (args[0] != "latest" && args[0] != "search")) {
		std::println(stderr, "{}: support needs a subcommand (latest|search)", program);
		return usage();
	}
	const std::string_view which = args[0];
	const std::optional<std::string_view> pkey = flag_value(args, "-p");
	if (!pkey) {
		std::println(stderr, "{}: support needs -p <parser>", program);
		return Usage;
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
	const CompatibilitiesFlags pflags = parser->compatibilities().flags;
	const bool is_manga  = pflags.has(supports_manga_store);
	const bool is_images = pflags.has(supports_images_store) || pflags.has(supports_images_search);
	if (!is_manga && !is_images) {
		std::println(stderr, "{}: parser '{}' has no browsable category", program, *pkey);
		return Usage;
	}
	const std::string source        = parser->info().name;
	const std::string_view category = is_manga ? "manga" : "images";

	if (which == "search") {
		std::optional<SearchCompatibilities> sc =
		    is_manga ? fetch_search_support(ready, parser->mangas_getter())
		             : fetch_search_support(ready, parser->images_getter());
		if (!sc) {
			return Runtime;
		}
		return print_search_support(source, category, *sc, json);
	}

	// latest — synchronous, no network.
	if (is_manga) {
		const MangaGetterRootCompatibilities s = parser->mangas_getter()->latest_support();
		return print_latest_support(source, category, s.supported_sorts, s.compatibilities, json);
	}
	const ImagesGetterRootCompatibilities s = parser->images_getter()->latest_support();
	return print_latest_support(source, category, s.supported_sorts, s.compatibilities, json);
}

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

struct DumpResult {
	int ok = 0;
	int failed = 0;
	boost::json::array written;
};

/// Fetch a container's items and write each image to @p dest via request()->body
/// (whole image buffered — fine for stills; video/huge waits on streaming). The
/// shared sink so a pasted URL (parse_url) and a piped serialize() handle
/// (from_serialized) both funnel here; per-file progress prints as it goes, and
/// the caller prints the final summary via report().
void dump_container(RequestorContext& ctx, ImageContainerGetter& container,
                    const std::string& dest, GetFilters filters, bool json, DumpResult& acc) {
	auto items = coro::sync_wait(container.items(ctx, filters));
	if (!items) {
		std::println(stderr, "{}: items failed: {}", program, items.error().message);
		++acc.failed;
		return;
	}

	std::error_code ec;
	std::filesystem::create_directories(dest, ec);

	for (auto& entry : items->results) {
		const Image& image = entry.item.image;
		auto resp = coro::sync_wait(ctx.request(GetRequest{ .url = image.url, .headers = image.headers }));
		if (!resp || resp->status_code >= 400 || resp->body.empty()) {
			++acc.failed;
			std::println(stderr, "{}: item {} failed{}", program, static_cast<long long>(entry.offset),
			    resp ? std::format(" (http {})", resp->status_code) : std::string{});
			continue;
		}
		const std::filesystem::path out =
		    std::filesystem::path(dest) / filename_from_url(image.url, entry.offset);
		std::ofstream file(out, std::ios::binary);
		if (!file) {
			++acc.failed;
			std::println(stderr, "{}: cannot open {}", program, out.string());
			continue;
		}
		file.write(resp->body.data(), static_cast<std::streamsize>(resp->body.size()));
		++acc.ok;
		if (json) {
			acc.written.emplace_back(out.string());
		} else {
			std::println("  {} ({} bytes)", out.string(), resp->body.size());
		}
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
                     const std::string& dest, GetFilters filters, bool json, DumpResult& acc) {
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
	if (!flags.has(supports_images_store) && !flags.has(supports_images_search)) {
		std::println(stderr, "{}: skipping non-image parser '{}'", program, pid);
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
	auto root = parser->images_getter();
	auto getter = coro::sync_wait(root->from_serialized(std::move(data)));
	if (!getter) {
		std::println(stderr, "{}: from_serialized failed for '{}': {}", program, pid, getter.error().message);
		++acc.failed;
		return;
	}
	dump_container(ready, **getter, dest, filters, json, acc);
}

int download(std::span<const std::string_view> args, bool json) {
	// First positional (non-flag) token is the URL; skip the value-taking flags so
	// their arguments aren't mistaken for it.
	std::optional<std::string_view> url;
	for (std::size_t i = 0; i < args.size(); ++i) {
		const std::string_view a = args[i];
		if (a == "--dest" || a == "--limit") {
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

	ParserStore store;
	populate_store(store);
	auto client = std::make_shared<AsyncClient>();
	RequestorContext ctx(client);

	// URL mode: a pasted container URL.
	if (url) {
		std::optional<UrlRoute> route = store.route_url(*url);
		if (!route) {
			std::println(stderr, "{}: no source handles '{}'", program, *url);
			return Runtime;
		}
		if (route->type != GetterSuggestionType::Images) {
			std::println(stderr, "{}: download currently supports image containers only", program);
			return Usage;
		}
		RequestorContext ready = ctx.new_with_config(route->parser->make_config(ctx.config()));
		auto root = route->parser->images_getter();
		auto getter = coro::sync_wait(root->parse_url(ready, std::move(route->url)));
		if (!getter) {
			std::println(stderr, "{}: parse failed: {}", program, getter.error().message);
			return Runtime;
		}
		DumpResult result;
		dump_container(ready, **getter, dest, filters, json, result);
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
		std::println(stderr, "{}: could not parse stdin as JSON: {}", program, e.what());
		return Usage;
	}

	DumpResult result;
	if (doc.is_array()) {
		for (const auto& v : doc.as_array()) {
			if (v.is_object()) {
				dump_serialized(store, ctx, v.as_object(), dest, filters, json, result);
			}
		}
	} else if (doc.is_object()) {
		dump_serialized(store, ctx, doc.as_object(), dest, filters, json, result);
	} else {
		std::println(stderr, "{}: stdin JSON must be an object or array of records", program);
		return Usage;
	}
	return report(std::move(result), dest, json);
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
	if (verb == "support") return support(verb_args, json);
	if (verb == "latest")  return latest(verb_args, json);
	if (verb == "search")  return search(verb_args, json);
	if (verb == "parse")   return parse_verb(verb_args, json);
	if (verb == "download") return download(verb_args, json);

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
