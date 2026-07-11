/*
 * Copyright (C) 2025-2026 Toilettrauma
 *
 * anip — a command-line interface to libaniparse. Not a downloader: the point is
 * automation over the whole library (search --json | jq | parse --download,
 * cron trackers on latest --json). All verbs are implemented; download handles
 * image containers by buffering each file (request()->body), with streaming/CBZ
 * and a serialized-handle input (to close search|download) still to come.
 *
 * This TU owns verb dispatch, the search/latest/support/parse verbs, and the shared
 * low-level helpers declared in CliCommon.hpp. The filter vocabulary lives in
 * CliFilters.cpp and the download machinery in CliDownload.cpp.
 */
#include "CliCommon.hpp"

#include <aniparse/ParserStore.hpp>
#include <aniparse/Client.hpp>
#include <aniparse/parsers/DefaultParsers.hpp>

#include "anip_extensions.hpp" // generated: register_extensions (seam A)

#include <boost/json.hpp>
#include <coro/sync_wait.hpp>

#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aniparse::cli {
using namespace aniparse;

int usage() {
	std::println(stderr,
	    "Usage: {0} [--help] [--json] <command> [<args> ...]\n"
	    "\n"
	    "Commands:\n"
	    "  list parsers            list the available sources\n"
	    "  list filters            search-filter vocabulary + --filter value syntax\n"
	    "  support (latest|search) -p <parser>   show what a source supports\n"
	    "  latest -p <parser> [--from N] [--limit N] [--sort KEY] [--asc]\n"
	    "  search -p <parser> [-q <query>] [--filter k=v ...] [--from N] [--limit N]\n"
	    "                          (--filter repeatable; k=!v excludes; needs -q or --filter)\n"
	    "  parse <url>             route a URL to its source and fetch info\n"
	    "  download [<url>] [--dest DIR] [--limit N] [--chapters 1-4,8] [--jobs N] [--delay MS]  save files\n"
	    "                          (image container, or manga chapters->pages;\n"
	    "                           no <url>: read search/latest --json records from stdin)\n"
	    "\n"
	    "Auth (search/latest/parse/download — a source that needs credentials):\n"
	    "  --token T               token login    (or env ANIP_<PARSER>_TOKEN)\n"
	    "  --user U --password P   user/pass login (or env ANIP_<PARSER>_USER / _PASSWORD)\n"
	    "                          <PARSER> = the id upper-cased, e.g. ANIP_GELBOORU_USER;\n"
	    "                          env keeps secrets out of argv. Gelbooru: user=user_id, password=api_key\n"
	    "\n"
	    "Global:\n"
	    "  --json   emit machine-readable JSON instead of human text\n"
	    "  --help   show this help and exit\n",
	    program);
	return Usage;
}

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

void populate_store(ParserStore& store) {
	parsers::emplace_default_parsers(store);
	register_extensions(store); // aniparse::cli::register_extensions (generated seam A)
}

/// The value of ANIP_<PARSER>_<suffix> (parser id upper-cased), or nullopt. The env
/// is the secret-safe channel: unlike --user/--token it never lands in argv or the
/// shell history.
std::optional<std::string> auth_env(std::string_view parser_id, std::string_view suffix) {
	std::string name = "ANIP_";
	for (const char c : parser_id) {
		name += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	name += '_';
	name += suffix;
#ifdef _MSC_VER
#	pragma warning(push)
#	pragma warning(disable : 4996) // std::getenv is the portable read; no writes, safe here
#endif
	if (const char* v = std::getenv(name.c_str())) {
		return std::string(v);
	}
#ifdef _MSC_VER
#	pragma warning(pop)
#endif
	return std::nullopt;
}

std::optional<AuthenticationData> resolve_credentials(std::string_view parser_id,
                                                      std::span<const std::string_view> args) {
	// A token wins outright when present (flag over env).
	std::optional<std::string> token;
	if (const auto t = flag_value(args, "--token")) {
		token = std::string(*t);
	} else {
		token = auth_env(parser_id, "TOKEN");
	}
	if (token) {
		return AuthenticationToken{ .token = std::move(*token), .type = {} };
	}

	// Otherwise a user/password pair (each flag falls back to its env var).
	std::optional<std::string> user;
	if (const auto u = flag_value(args, "--user")) {
		user = std::string(*u);
	} else {
		user = auth_env(parser_id, "USER");
	}
	std::optional<std::string> password;
	if (const auto p = flag_value(args, "--password")) {
		password = std::string(*p);
	} else {
		password = auth_env(parser_id, "PASSWORD");
	}
	if (user && password) {
		return AuthenticationUserPassword{ .username = std::move(*user), .password = std::move(*password) };
	}
	if (user || password) {
		std::println(stderr, "{}: both --user and --password (or ANIP_{}_USER/_PASSWORD) are required "
		    "— continuing unauthenticated", program, "<PARSER>");
	}
	return std::nullopt;
}

std::optional<RequestorContext> make_ready_context(const RequestorContext& base, Parser& parser,
                                                   std::span<const std::string_view> args) {
	// Always derive the parser's own config first (parser identity + defaults).
	RequestorContext ready = base.new_with_config(parser.make_config(base.config()));

	std::optional<AuthenticationData> creds = resolve_credentials(parser.identifier(), args);
	if (!creds) {
		return ready; // anonymous — no credentials supplied
	}

	// authenticate_context returns a FRESH const config and leaves `ready` untouched;
	// adopt it by copying into a mutable config (new_with_config wants a non-const one).
	auto authed = coro::sync_wait(parser.authenticate_context(ready, std::move(*creds)));
	if (!authed) {
		std::println(stderr, "{}: authentication failed for '{}': {}", program,
		    parser.identifier(), authed.error().message);
		return std::nullopt;
	}
	return ready.new_with_config(std::make_shared<ParserConfig>(**authed));
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
		return list_filters(json);
	}
	std::println(stderr, "{}: unknown list subcommand '{}'", program, args[0]);
	return usage();
}

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

/// Run a search on @p root (manga or images): fetch its declared support, build the
/// structured --filter set typed against it, and emit the page. Templated so one
/// body serves both root getter kinds (both expose search_support + search).
template <typename Root>
int run_search(RequestorContext& ready, Root& root, std::string_view query,
               std::span<const std::string_view> args, const GetFilters& filters,
               std::string_view parser_id, bool json) {
	auto support = coro::sync_wait(root.search_support(ready));
	if (!support) {
		std::println(stderr, "{}: search_support failed: {}", program, support.error().message);
		return Runtime;
	}
	std::optional<SearchItems> structured = build_search_filters(args, support->supported_filters);
	if (!structured) {
		return Usage;
	}

	SearchRequestQuery request;
	request.query   = std::string(query);
	request.filters = std::move(*structured);

	// Central pre-flight before any network: unknown/type-mismatched filters,
	// unsupported exclusion, out-of-set selection tokens (an empty support selection
	// is open vocabulary and accepts any token), and the sort key/direction.
	if (auto errors = validate_query(*support, request, filters); !errors.empty()) {
		std::println(stderr, "{}: {}", program, describe_search_query_errors(errors));
		return Usage;
	}
	return emit_page(ready, coro::sync_wait(root.search(ready, request, filters)), parser_id, json);
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
	auto ready_opt = make_ready_context(ctx, *parser, args);
	if (!ready_opt) {
		return Runtime;
	}
	RequestorContext ready = std::move(*ready_opt);

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
	const bool has_filter = has_flag(args, "--filter");
	if (!query && !has_filter) {
		std::println(stderr, "{}: search needs -q <query> or --filter k=v", program);
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
	auto ready_opt = make_ready_context(ctx, *parser, args);
	if (!ready_opt) {
		return Runtime;
	}
	RequestorContext ready = std::move(*ready_opt);

	const std::string_view q = query.value_or(std::string_view{});
	using namespace compatibilities_flags;
	const CompatibilitiesFlags flags = parser->compatibilities().flags;
	if (flags.has(supports_manga_store)) {
		auto root = parser->mangas_getter();
		return run_search(ready, *root, q, args, *filters, parser->identifier(), json);
	}
	if (flags.has(supports_images_search)) {
		auto root = parser->images_getter();
		return run_search(ready, *root, q, args, *filters, parser->identifier(), json);
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
	for (std::size_t i = 0; i < args.size(); ++i) {
		const std::string_view a = args[i];
		if (a == "--token" || a == "--user" || a == "--password") {
			++i; // skip the flag's value so it isn't mistaken for the URL
			continue;
		}
		if (a.starts_with('-')) {
			continue;
		}
		url = a;
		break;
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
	auto ready_opt = make_ready_context(ctx, *route->parser, args);
	if (!ready_opt) {
		return Runtime;
	}
	RequestorContext ready = std::move(*ready_opt);
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
	auto ready_opt = make_ready_context(ctx, *parser, args);
	if (!ready_opt) {
		return Runtime;
	}
	RequestorContext ready = std::move(*ready_opt);

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
} // namespace aniparse::cli

int main(int argc, const char** argv) {
	std::vector<std::string_view> args;
	args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
	for (int i = 1; i < argc; ++i) {
		args.emplace_back(argv[i]);
	}

	try {
		return aniparse::cli::real_main(args);
	}
	catch (const std::exception& e) {
		std::println(stderr, "{}: error: {}", aniparse::cli::program, e.what());
		return aniparse::cli::Runtime;
	}
	catch (...) {
		std::println(stderr, "{}: unknown error", aniparse::cli::program);
		return aniparse::cli::Runtime;
	}
}
