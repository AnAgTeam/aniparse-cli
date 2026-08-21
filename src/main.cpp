/*
 * Copyright (C) 2025-2026 Toilettrauma
 *
 * anip — a command-line interface to libaniparse. Not a downloader: the point is
 * automation over the whole library (search --json | jq | parse --download,
 * cron trackers on latest --json). All verbs are implemented; download handles
 * image containers by buffering each file (request()->body), with streaming/CBZ
 * and a serialized-handle input (to close search|download) still to come.
 *
 * This TU owns verb dispatch, the search/latest/support/parse/episodes verbs, and the shared
 * low-level helpers declared in CliCommon.hpp. The filter vocabulary lives in
 * CliFilters.cpp and the download machinery in CliDownload.cpp.
 */
#include "CliCommon.hpp"

#include <aniparse/ParserStore.hpp>
#include <aniparse/net/Client.hpp>
#include <aniparse/anime/Anime.hpp>
#include <aniparse/parsers/DefaultParsers.hpp>
#include <aniparse/video/VideoExtractor.hpp>

#include "anip_extensions.hpp" // generated: register_extensions (seam A)

#include <boost/json.hpp>
#include <coro/sync_wait.hpp>

#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <iterator>
#include <memory>
#include <limits>
#include <optional>
#include <print>
#include <span>
#include <sstream>
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
	    "  list extractors         list the available video extractors\n"
	    "  list filters            search-filter vocabulary + --filter value syntax\n"
	    "  support (latest|search) -p <parser>   show what a source supports\n"
	    "  latest -p <parser> [--from N] [--limit N] [--sort KEY] [--asc]\n"
	    "  search -p <parser> [-q <query>] [--filter k=v ...] [--from N] [--limit N]\n"
	    "                          (--filter repeatable; k=!v excludes; needs -q or --filter)\n"
	    "  parse <url>             route a URL to its source and fetch info\n"
	    "  episodes <url> [--track ID] [--episode N] [--limit N]\n"
	    "                          list anime tracks, episodes, or episode sources\n"
	    "                          (<url> may be '-' to read search/latest --json records from stdin)\n"
	    "  extract (<url>|-)       route an external player URL to its video extractor\n"
	    "                          and print the resolved streams / delegated links\n"
	    "                          ('-': read player URLs from stdin, one per line)\n"
	    "  catalog apply <file>    validate and cache a volatile catalog file (dev only:\n"
	    "                          signature check stubbed; every later run applies it)\n"
	    "  catalog status          show the cached catalog summary\n"
	    "  catalog clear           drop the cached catalog\n"
	    "  download (<url>|-) [--dest DIR] [--limit N] [--chapters 1-4,8] [--jobs N] [--delay MS]  save files\n"
	    "                          (image container, or manga chapters->pages;\n"
	    "                           '-': read search/latest --json records from stdin)\n"
	    "\n"
	    "Auth (search/latest/parse/episodes/download — a source that needs credentials):\n"
	    "  --token T               token login    (or env ANIP_<PARSER>_TOKEN)\n"
	    "  --user U --password P   user/pass login (or env ANIP_<PARSER>_USER / _PASSWORD)\n"
	    "                          <PARSER> = the id upper-cased, e.g. ANIP_GELBOORU_USER;\n"
	    "                          env keeps secrets out of argv. Gelbooru: user=user_id, password=api_key\n"
	    "\n"
	    "Global:\n"
	    "  --json   emit machine-readable JSON instead of human text\n"
	    "  --help   show this help and exit\n"
	    "\n"
	    "Environment:\n"
	    "  ANIP_CATALOG   override the catalog cache path (default: anip-catalog.json\n"
	    "                 next to the executable)\n",
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
	// One batched edit: the showcase set and every extension registrar add to it,
	// and the single commit rebuilds the routing snapshot once.
	auto edit = store.begin_edit();
	parsers::emplace_default_parsers(edit);
	register_extensions(edit); // aniparse::cli::register_extensions (generated seam A)
	edit.commit();
}

/// Build the video-extractor store from the extension extractor registrars
/// linked into this build (seam A). File-local: only the extract verbs use it.
void populate_extractors(VideoExtractorStore& store) {
	auto edit = store.begin_edit();
	register_extension_extractors(edit); // aniparse::cli::register_extension_extractors (generated seam A)
	edit.commit();
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

/// A collecting EmplaceDomainsContext for `list extractors`: gathers the bare
/// hosts an extractor declares, without building a routing index out of them.
struct DomainCollector final : EmplaceDomainsContext {
	void add_domain(std::string_view domain) override {
		domains.emplace_back(domain);
	}
	std::vector<std::string> domains;
};

int list_extractors(bool json) {
	VideoExtractorStore store;
	populate_extractors(store);
	const std::vector<std::shared_ptr<VideoExtractor>> extractor_list = store.extractors();

	if (json) {
		boost::json::array arr;
		for (const std::shared_ptr<VideoExtractor>& e : extractor_list) {
			DomainCollector collector;
			e->emplace_domains(collector);
			boost::json::array hosts;
			for (const std::string& d : collector.domains) {
				hosts.emplace_back(d);
			}
			boost::json::object o;
			o["id"]      = e->identifier();
			o["domains"] = std::move(hosts);
			arr.push_back(std::move(o));
		}
		std::println("{}", boost::json::serialize(boost::json::value(std::move(arr))));
		return Ok;
	}

	for (const std::shared_ptr<VideoExtractor>& e : extractor_list) {
		DomainCollector collector;
		e->emplace_domains(collector);
		std::vector<std::string_view> hosts;
		for (const std::string& d : collector.domains) {
			hosts.push_back(d);
		}
		std::println("{:<12} {}", e->identifier(), join(hosts, ", "));
	}
	return Ok;
}

int list(std::span<const std::string_view> args, bool json) {
	if (args.empty()) {
		std::println(stderr, "{}: list needs a subcommand (parsers|extractors|filters)", program);
		return usage();
	}
	if (args[0] == "parsers") {
		return list_parsers(json);
	}
	if (args[0] == "extractors") {
		return list_extractors(json);
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

/// Print a fetched page of item getters (shared by latest and search). Every
/// current domain returns an optional, synchronous preview card; listing a page
/// must never turn into a details request per row. In --json each row
/// also carries the parser id + the item's serialize() handle, so the output
/// pipes straight into `download` (which rebuilds the getter via from_serialized).
template <typename PageResult>
int emit_page(RequestorContext&, PageResult page, std::string_view parser_id,
              std::string_view category, bool json) {
	if (!page) {
		std::println(stderr, "{}: request failed: {}", program, page.error().message);
		return Runtime;
	}

	boost::json::array arr;
	int skipped = 0;
	for (auto& entry : page->results) {
		auto info = entry.item->preview_info();
		if (!info) {
			++skipped; // URL/restored getters have no cheap card; do not fetch details here
			continue;
		}
		if (json) {
			boost::json::object o;
			o["offset"] = static_cast<std::int64_t>(entry.offset);
			o["title"]  = info->common.title;
			o["parser"] = std::string(parser_id);
			o["category"] = std::string(category);
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
			std::println("{:>4}  {}", static_cast<long long>(entry.offset), info->common.title);
		}
	}

	if (json) {
		std::println("{}", boost::json::serialize(boost::json::value(std::move(arr))));
	} else if (skipped > 0) {
		std::println(stderr, "{}: {} item(s) skipped (source supplied no preview)", program, skipped);
	}
	return Ok;
}

/// Run a search on @p root (manga or images): fetch its declared support, build the
/// structured --filter set typed against it, and emit the page. Templated so one
/// body serves both root getter kinds (both expose search_support + search).
template <typename Root>
int run_search(RequestorContext& ready, Root& root, std::string_view query,
               std::span<const std::string_view> args, const GetFilters& filters,
               std::string_view parser_id, std::string_view category, bool json) {
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
	return emit_page(ready, coro::sync_wait(root.search(ready, request, filters)), parser_id, category, json);
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
	CatalogServices services;
	apply_cached_catalog(&store, nullptr, services);
	const std::shared_ptr<Parser> parser = store.find_by_key(*pkey);
	if (!parser) {
		std::println(stderr, "{}: no parser '{}' (try `{} list parsers`)", program, *pkey, program);
		return Usage;
	}

	RequestorContext ctx = services.parser_context();
	auto ready_opt = make_ready_context(ctx, *parser, args);
	if (!ready_opt) {
		return Runtime;
	}
	RequestorContext ready = std::move(*ready_opt);

	using namespace compatibilities_flags;
	const CompatibilitiesFlags flags = parser->compatibilities().flags;
	if (flags.has(supports_manga_store)) {
		auto root = parser->mangas_getter();
		return emit_page(ready, coro::sync_wait(root->latest(ready, *filters)), parser->identifier(), "manga", json);
	}
	if (flags.has(supports_anime_store)) {
		auto root = parser->animes_getter();
		return emit_page(ready, coro::sync_wait(root->latest(ready, *filters)), parser->identifier(), "anime", json);
	}
	if (flags.has(supports_images_store) || flags.has(supports_images_search)) {
		auto root = parser->images_getter();
		return emit_page(ready, coro::sync_wait(root->latest(ready, *filters)), parser->identifier(), "images", json);
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
	CatalogServices services;
	apply_cached_catalog(&store, nullptr, services);
	const std::shared_ptr<Parser> parser = store.find_by_key(*pkey);
	if (!parser) {
		std::println(stderr, "{}: no parser '{}' (try `{} list parsers`)", program, *pkey, program);
		return Usage;
	}

	RequestorContext ctx = services.parser_context();
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
		return run_search(ready, *root, q, args, *filters, parser->identifier(), "manga", json);
	}
	if (flags.has(supports_anime_store)) {
		auto root = parser->animes_getter();
		return run_search(ready, *root, q, args, *filters, parser->identifier(), "anime", json);
	}
	if (flags.has(supports_images_search)) {
		auto root = parser->images_getter();
		return run_search(ready, *root, q, args, *filters, parser->identifier(), "images", json);
	}
	std::println(stderr, "{}: parser '{}' does not support search", program, *pkey);
	return Usage;
}

/// Print full info for a parsed manga, anime, or image container. All current
/// media models carry shared fields through `common`; their own numeric id stays
/// on the domain wrapper.
template <typename GetterPtr>
int emit_parsed(RequestorContext& ctx, GetterPtr getter, std::string_view source,
                std::string_view category, bool json, boost::json::array* json_out = nullptr) {
	auto info = coro::sync_wait(getter->info(ctx));
	if (!info) {
		std::println(stderr, "{}: fetch failed: {}", program, info.error().message);
		return Runtime;
	}

	if (json) {
		boost::json::array tags;
		for (const Tag& t : info->common.tags) {
			tags.emplace_back(t.name);
		}
		boost::json::object o;
		o["source"]      = std::string(source);
		o["category"]    = std::string(category);
		o["id"]          = static_cast<std::int64_t>(info->id);
		 o["title"]       = info->common.title;
		 o["description"] = info->common.description.text;
		 o["tags"]        = std::move(tags);
		if (json_out) {
			json_out->push_back(std::move(o));
		} else {
			std::println("{}", boost::json::serialize(boost::json::value(std::move(o))));
		}
		return Ok;
	}

	std::println("Source:   {} ({})", source, category);
	std::println("Id:       {}", static_cast<long long>(info->id));
	std::println("Title:    {}", info->common.title);
	if (!info->common.description.text.empty()) {
		std::println("Summary:  {}", info->common.description.text);
	}
	if (!info->common.tags.empty()) {
		std::vector<std::string_view> names;
		for (const Tag& t : info->common.tags) {
			names.push_back(t.name);
		}
		std::println("Tags:     {}", join(names, ", "));
	}
	return Ok;
}

/// One validated search/latest --json stdin record: the parser it names, its
/// category, and the SerializedGetterData unpacked from its handle.
struct StdinRecord {
	std::shared_ptr<Parser> parser;
	std::string category;
	SerializedGetterData data;
};

/// Validate and unpack one search/latest --json stdin record. Prints the reason
/// and returns the failing exit code when parser, category, or handle is missing,
/// malformed, or names an unknown parser.
std::expected<StdinRecord, int> unpack_stdin_record(ParserStore& store, const boost::json::object& record) {
	const auto* parser_value = record.if_contains("parser");
	const auto* category_value = record.if_contains("category");
	const auto* handle_value = record.if_contains("handle");
	if (!parser_value || !parser_value->is_string() || !category_value || !category_value->is_string()
	 || !handle_value || !handle_value->is_object()) {
		std::println(stderr, "{}: stdin record needs parser, category, and handle", program);
		return std::unexpected(Usage);
	}
	const std::string parser_id(parser_value->as_string().c_str());
	const std::shared_ptr<Parser> parser = store.find_by_key(parser_id);
	if (!parser) {
		std::println(stderr, "{}: no parser '{}' for stdin record", program, parser_id);
		return std::unexpected(Runtime);
	}
	StdinRecord unpacked;
	unpacked.parser = parser;
	unpacked.category = category_value->as_string().c_str();
	const boost::json::object& handle = handle_value->as_object();
	if (const auto* url = handle.if_contains("url"); url && url->is_string()) {
		unpacked.data.url = url->as_string().c_str();
	}
	if (const auto* params = handle.if_contains("params"); params && params->is_object()) {
		for (const auto& [key, value] : params->as_object()) {
			if (value.is_string()) unpacked.data.params[std::string(key)] = value.as_string().c_str();
		}
	}
	return unpacked;
}

/// Rebuild and print one getter carried by search/latest --json. The category is
/// part of the record so a multi-domain parser never has to guess which root owns
/// an opaque handle.
int parse_serialized_record(ParserStore& store, RequestorContext& ctx,
                            const boost::json::object& record,
                            std::span<const std::string_view> args, bool json,
                            boost::json::array* json_out) {
	auto unpacked = unpack_stdin_record(store, record);
	if (!unpacked) return unpacked.error();
	const std::string& category = unpacked->category;
	const std::shared_ptr<Parser>& parser = unpacked->parser;
	auto ready_opt = make_ready_context(ctx, *parser, args);
	if (!ready_opt) return Runtime;
	RequestorContext ready = std::move(*ready_opt);
	const std::string source = parser->info().name;
	if (category == "manga") {
		auto getter = coro::sync_wait(parser->mangas_getter()->from_serialized(std::move(unpacked->data)));
		if (!getter) {
			std::println(stderr, "{}: from_serialized failed: {}", program, getter.error().message);
			return Runtime;
		}
		return emit_parsed(ready, std::move(*getter), source, category, json, json_out);
	}
	if (category == "anime") {
		auto getter = coro::sync_wait(parser->animes_getter()->from_serialized(std::move(unpacked->data)));
		if (!getter) {
			std::println(stderr, "{}: from_serialized failed: {}", program, getter.error().message);
			return Runtime;
		}
		return emit_parsed(ready, std::move(*getter), source, category, json, json_out);
	}
	if (category == "images") {
		auto getter = coro::sync_wait(parser->images_getter()->from_serialized(std::move(unpacked->data)));
		if (!getter) {
			std::println(stderr, "{}: from_serialized failed: {}", program, getter.error().message);
			return Runtime;
		}
		return emit_parsed(ready, std::move(*getter), source, category, json, json_out);
	}
	std::println(stderr, "{}: unsupported stdin category '{}'", program, category);
	return Usage;
}

int parse_stdin(std::span<const std::string_view> args, bool json) {
	std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
	if (input.starts_with("\xEF\xBB\xBF")) input.erase(0, 3);
	if (input.find_first_not_of(" \t\r\n") == std::string::npos) {
		std::println(stderr, "{}: parse - needs JSON from search/latest --json on stdin", program);
		return Usage;
	}
	boost::json::value doc;
	try {
		doc = boost::json::parse(input);
	} catch (const std::exception& error) {
		std::println(stderr, "{}: stdin is not valid JSON ({})", program, error.what());
		return Usage;
	}
	ParserStore store;
	populate_store(store);
	CatalogServices services;
	apply_cached_catalog(&store, nullptr, services);
	RequestorContext ctx = services.parser_context();
	int result = Ok;
	boost::json::array parsed;
	auto parse_one = [&](const boost::json::value& value) {
		if (!value.is_object()) {
			std::println(stderr, "{}: stdin JSON entries must be objects", program);
			result = Usage;
			return;
		}
		const int code = parse_serialized_record(store, ctx, value.as_object(), args, json,
		                                          json ? &parsed : nullptr);
		if (code != Ok) result = code;
	};
	if (doc.is_array()) {
		for (const auto& value : doc.as_array()) parse_one(value);
	} else {
		parse_one(doc);
	}
	if (json) std::println("{}", boost::json::serialize(parsed));
	return result;
}

int parse_verb(std::span<const std::string_view> args, bool json) {
	std::optional<std::string_view> url;
	for (std::size_t i = 0; i < args.size(); ++i) {
		const std::string_view a = args[i];
		if (a == "--token" || a == "--user" || a == "--password") {
			++i; // skip the flag's value so it isn't mistaken for the URL
			continue;
		}
		if (a != "-" && a.starts_with('-')) {
			continue;
		}
		url = a;
		break;
	}
	if (!url) {
		std::println(stderr, "{}: parse needs a <url>, or '-' for search/latest JSON on stdin", program);
		return Usage;
	}
	if (*url == "-") return parse_stdin(args, json);

	ParserStore store;
	populate_store(store);
	CatalogServices services;
	apply_cached_catalog(&store, nullptr, services);
	std::optional<UrlRoute> route = store.route_url(*url);
	if (!route) {
		std::println(stderr, "{}: no source handles '{}' (try `{} list parsers`)",
		    program, *url, program);
		return Runtime;
	}

	RequestorContext ctx = services.parser_context();
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
	if (route->type == GetterSuggestionType::Anime) {
		auto root = route->parser->animes_getter();
		auto getter = coro::sync_wait(root->parse_url(ready, std::move(route->url)));
		if (!getter) {
			std::println(stderr, "{}: parse failed: {}", program, getter.error().message);
			return Runtime;
		}
		return emit_parsed(ready, std::move(*getter), source, "anime", json);
	}
	std::println(stderr, "{}: '{}' routed to {} but its category is not supported yet",
	    program, *url, source);
	return Runtime;
}

/// Resolve one external player URL through the video-extractor store: route it,
/// extract it (chasing delegated extractor→extractor links), and print the
/// direct streams and/or unresolved delegated links it returns. When json_out
/// is set the JSON row is appended there instead of printed, so the stdin loop
/// can aggregate several URLs into one array.
int extract_one(VideoExtractorStore& store, RequestorContext& ctx, std::string_view url,
                bool json, boost::json::array* json_out) {
	std::optional<VideoExtractionRoute> route = store.route_url(url);
	if (!route) {
		std::println(stderr, "{}: no extractor handles '{}' (try `{} list extractors`)",
		    program, url, program);
		return Runtime;
	}

	const std::string extractor_id = route->extractor->identifier();
	// store.extract re-routes internally but also chases delegated links
	// (extractor→extractor handoffs), which a bare route->extractor->extract
	// would not follow.
	auto extraction = coro::sync_wait(store.extract(ctx, std::string(url)));
	if (!extraction) {
		std::println(stderr, "{}: extract failed: {}", program, extraction.error().message);
		return Runtime;
	}

	if (json) {
		boost::json::array streams;
		for (const VideoStream& stream : extraction->streams) {
			boost::json::object value;
			value["url"]     = stream.url;
			value["quality"] = stream.quality;
			value["hls"]     = stream.is_hls;
			streams.push_back(std::move(value));
		}
		boost::json::array links;
		for (const VideoExtractionLink& link : extraction->links) {
			boost::json::object value;
			value["url"] = link.url;
			if (link.extractor_id) value["extractor"] = *link.extractor_id;
			links.push_back(std::move(value));
		}
		boost::json::object headers;
		for (const auto& [name, value] : extraction->headers) {
			headers[name] = value;
		}
		boost::json::object out;
		out["url"]       = std::string(url);
		out["extractor"] = extractor_id;
		out["streams"]   = std::move(streams);
		out["links"]     = std::move(links);
		out["headers"]   = std::move(headers);
		if (json_out) json_out->push_back(std::move(out));
		else std::println("{}", boost::json::serialize(boost::json::value(std::move(out))));
	} else {
		std::println("Extractor: {}", extractor_id);
		for (const VideoStream& stream : extraction->streams) {
			std::println("{:>4}  {}{}", stream.quality, stream.url, stream.is_hls ? "  (hls)" : "");
		}
		for (const VideoExtractionLink& link : extraction->links) {
			std::println("  -> {}{}", link.url,
			    link.extractor_id ? "  (via " + *link.extractor_id + ")" : std::string{});
		}
	}
	if (extraction->streams.empty() && extraction->links.empty()) {
		std::println(stderr, "{}: extractor '{}' resolved no streams or links", program, extractor_id);
		return Runtime;
	}
	return Ok;
}

/// extract '-' mode: read external player URLs from stdin, one per line (blank
/// lines skipped) — e.g. piped from `episodes --episode --json` via
/// `jq -r '.sources[].stream_url'`. In --json mode the per-URL rows aggregate
/// into one array, mirroring `parse -`.
int extract_stdin(bool json) {
	std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
	if (input.starts_with("\xEF\xBB\xBF")) input.erase(0, 3);
	if (input.find_first_not_of(" \t\r\n") == std::string::npos) {
		std::println(stderr, "{}: extract - needs player URLs on stdin, one per line", program);
		return Usage;
	}
	VideoExtractorStore store;
	populate_extractors(store);
	CatalogServices services;
	apply_cached_catalog(nullptr, &store, services);
	RequestorContext ctx = services.extractor_context();
	int result = Ok;
	boost::json::array rows;
	std::istringstream lines(input);
	for (std::string line; std::getline(lines, line);) {
		const std::size_t first = line.find_first_not_of(" \t\r");
		if (first == std::string::npos) continue;
		const std::size_t last = line.find_last_not_of(" \t\r");
		const int code = extract_one(store, ctx, std::string_view(line).substr(first, last - first + 1),
		                             json, json ? &rows : nullptr);
		if (code != Ok) result = code;
	}
	if (json) std::println("{}", boost::json::serialize(rows));
	return result;
}

/// Resolve external player URLs into playable streams. Unlike parse there is no
/// source config or auth — extractors are standalone URL handlers, so an
/// anonymous base context is enough. <url> may be '-' to read URLs from stdin.
int extract(std::span<const std::string_view> args, bool json) {
	std::optional<std::string_view> url;
	for (const std::string_view a : args) {
		if (a != "-" && a.starts_with('-')) {
			continue; // no value-taking flags on this verb yet
		}
		url = a;
		break;
	}
	if (!url) {
		std::println(stderr, "{}: extract needs an external player <url>, or '-' to read URLs from stdin", program);
		return Usage;
	}
	if (*url == "-") return extract_stdin(json);

	VideoExtractorStore store;
	populate_extractors(store);
	CatalogServices services;
	apply_cached_catalog(nullptr, &store, services);
	RequestorContext ctx = services.extractor_context();
	return extract_one(store, ctx, *url, json, nullptr);
}

/// Run the track/episode/source listing for one already-built anime getter.
/// When json_out is set the JSON row is appended there instead of printed, so
/// the stdin loop can aggregate several records into one array.
int emit_episodes_hierarchy(RequestorContext& ready, AnimeGetter& getter,
                            const std::string& source, const GetFilters& filters,
                            std::optional<AnimeTrackID> track, std::optional<long> episode,
                            bool json, boost::json::array* json_out) {
	using namespace compatibilities_flags;
	const bool track_first = getter.compatibilities().flags.has(supports_tracks);
	if (!track && track_first) {
		auto page = coro::sync_wait(getter.tracks(ready, filters));
		if (!page) {
			std::println(stderr, "{}: tracks failed: {}", program, page.error().message);
			return Runtime;
		}
		if (json) {
			boost::json::array tracks;
			for (const auto& entry : page->results) {
				const AnimeTrackInfo& item = entry.item;
				boost::json::object row;
				row["id"] = item.id;
				row["team"] = item.team.name;
				row["player"] = item.player;
				if (item.episode_count) row["episode_count"] = *item.episode_count;
				tracks.push_back(std::move(row));
			}
			boost::json::object out;
			out["source"] = source;
			out["tracks"] = std::move(tracks);
			if (json_out) json_out->push_back(std::move(out));
			else std::println("{}", boost::json::serialize(out));
		} else {
			std::println("Source: {}", source);
			for (const auto& entry : page->results) {
				const AnimeTrackInfo& item = entry.item;
				std::println("{}  {} / {}{}", item.id, item.team.name, item.player,
				    item.episode_count ? std::format(" ({} episodes)", *item.episode_count) : std::string{});
			}
		}
		return Ok;
	}
	if (episode && track_first && !track) {
		std::println(stderr, "{}: this source requires --track before --episode", program);
		return Usage;
	}
	if (!episode) {
		auto page = coro::sync_wait(getter.episodes_info(ready, filters, track));
		if (!page) {
			std::println(stderr, "{}: episodes failed: {}", program, page.error().message);
			return Runtime;
		}
		if (json) {
			boost::json::array episodes;
			for (const auto& entry : page->results) {
				const AnimeEpisodeInfo& item = entry.item;
				boost::json::object row;
				row["episode"] = item.episode;
				row["number"] = item.number;
				row["title"] = item.name;
				episodes.push_back(std::move(row));
			}
			boost::json::object out;
			out["source"] = source;
			if (track) out["track"] = *track;
			out["episodes"] = std::move(episodes);
			if (json_out) json_out->push_back(std::move(out));
			else std::println("{}", boost::json::serialize(out));
		} else {
			std::println("Source: {}", source);
			for (const auto& entry : page->results) {
				const AnimeEpisodeInfo& item = entry.item;
				std::println("{}  {}", item.number.empty() ? std::to_string(item.episode) : item.number, item.name);
			}
		}
		return Ok;
	}

	// Episode numbers are display hints, not identities. Re-read the (unbounded)
	// listing and round-trip its ref so parsers with an opaque episode id receive
	// exactly the value they emitted.
	auto episodes_page = coro::sync_wait(getter.episodes_info(ready, GetFilters{}, track));
	if (!episodes_page) {
		std::println(stderr, "{}: episodes failed: {}", program, episodes_page.error().message);
		return Runtime;
	}
	const AnimeEpisodeInfo* selected = nullptr;
	for (const auto& entry : episodes_page->results) {
		if (entry.item.episode == *episode) {
			selected = &entry.item;
			break;
		}
	}
	if (!selected) {
		std::println(stderr, "{}: episode {} was not found", program, *episode);
		return Usage;
	}
	auto page = coro::sync_wait(getter.episode_sources(ready, selected->ref(), filters, track));
	if (!page) {
		std::println(stderr, "{}: episode sources failed: {}", program, page.error().message);
		return Runtime;
	}
	if (json) {
		boost::json::array sources;
		for (const auto& entry : page->results) {
			const VideoSource& item = entry.item;
			boost::json::object row;
			row["player"] = item.player;
			row["stream_url"] = item.stream_url;
			boost::json::array streams;
			for (const VideoStream& stream : item.streams) {
				boost::json::object value;
				value["url"] = stream.url;
				value["quality"] = stream.quality;
				value["hls"] = stream.is_hls;
				streams.push_back(std::move(value));
			}
			row["streams"] = std::move(streams);
			sources.push_back(std::move(row));
		}
		boost::json::object out;
		out["source"] = source;
		out["episode"] = *episode;
		if (track) out["track"] = *track;
		out["sources"] = std::move(sources);
		if (json_out) json_out->push_back(std::move(out));
		else std::println("{}", boost::json::serialize(out));
	} else {
		std::println("Source: {} — episode {}", source, *episode);
		for (const auto& entry : page->results) {
			const VideoSource& item = entry.item;
			std::println("{}  {}", item.player, item.stream_url);
		}
	}
	return Ok;
}

/// episodes stdin mode: rebuild each search/latest --json anime record and list
/// its track/episode hierarchy, mirroring `parse -`. Non-anime records are
/// rejected; in --json mode the per-record rows aggregate into one array.
int episodes_stdin(std::span<const std::string_view> args, bool json, const GetFilters& filters,
                   std::optional<AnimeTrackID> track, std::optional<long> episode) {
	std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
	if (input.starts_with("\xEF\xBB\xBF")) input.erase(0, 3);
	if (input.find_first_not_of(" \t\r\n") == std::string::npos) {
		std::println(stderr, "{}: episodes - needs JSON from search/latest --json on stdin", program);
		return Usage;
	}
	boost::json::value doc;
	try {
		doc = boost::json::parse(input);
	} catch (const std::exception& error) {
		std::println(stderr, "{}: stdin is not valid JSON ({})", program, error.what());
		return Usage;
	}
	ParserStore store;
	populate_store(store);
	CatalogServices services;
	apply_cached_catalog(&store, nullptr, services);
	RequestorContext ctx = services.parser_context();
	int result = Ok;
	boost::json::array rows;
	auto episodes_one = [&](const boost::json::value& value) {
		if (!value.is_object()) {
			std::println(stderr, "{}: stdin JSON entries must be objects", program);
			result = Usage;
			return;
		}
		auto unpacked = unpack_stdin_record(store, value.as_object());
		if (!unpacked) {
			result = unpacked.error();
			return;
		}
		if (unpacked->category != "anime") {
			std::println(stderr, "{}: episodes - only handles anime records (got '{}')",
			    program, unpacked->category);
			result = Usage;
			return;
		}
		auto ready_opt = make_ready_context(ctx, *unpacked->parser, args);
		if (!ready_opt) {
			result = Runtime;
			return;
		}
		RequestorContext ready = std::move(*ready_opt);
		const std::string source = unpacked->parser->info().name;
		auto getter = coro::sync_wait(unpacked->parser->animes_getter()->from_serialized(std::move(unpacked->data)));
		if (!getter) {
			std::println(stderr, "{}: from_serialized failed: {}", program, getter.error().message);
			result = Runtime;
			return;
		}
		const int code = emit_episodes_hierarchy(ready, **getter, source, filters, track, episode,
		    json, json ? &rows : nullptr);
		if (code != Ok) result = code;
	};
	if (doc.is_array()) {
		for (const auto& value : doc.as_array()) episodes_one(value);
	} else {
		episodes_one(doc);
	}
	if (json) std::println("{}", boost::json::serialize(rows));
	return result;
}

/// Inspect an anime's track-first or episode-first playback hierarchy without
/// downloading or resolving media. With no selector it lists tracks; --track
/// lists that track's episodes; --track plus --episode lists its advertised
/// VideoSource descriptors. Episode-first sources omit --track entirely.
/// <url> may be '-' to consume search/latest --json anime records from stdin.
int episodes(std::span<const std::string_view> args, bool json) {
	std::optional<std::string_view> url;
	for (std::size_t i = 0; i < args.size(); ++i) {
		const std::string_view a = args[i];
		if (a == "--track" || a == "--episode" || a == "--limit" || a == "--from"
		 || a == "--sort" || a == "--token" || a == "--user" || a == "--password") {
			++i;
			continue;
		}
		if (a != "-" && a.starts_with('-')) {
			continue;
		}
		url = a;
		break;
	}
	if (!url) {
		std::println(stderr, "{}: episodes needs an anime <url>, or '-' for search/latest JSON on stdin", program);
		return Usage;
	}
	const std::optional<GetFilters> filters = parse_filters(args);
	if (!filters) return Usage;

	std::optional<AnimeTrackID> track;
	if (const auto raw = flag_value(args, "--track")) {
		const auto value = parse_uint(*raw);
		if (!value || *value > std::numeric_limits<AnimeTrackID>::max()) {
			std::println(stderr, "{}: --track expects a non-negative 32-bit id", program);
			return Usage;
		}
		track = static_cast<AnimeTrackID>(*value);
	}
	std::optional<long> episode;
	if (const auto raw = flag_value(args, "--episode")) {
		const auto value = parse_uint(*raw);
		if (!value || *value < 1 || *value > std::numeric_limits<long>::max()) {
			std::println(stderr, "{}: --episode expects a positive integer", program);
			return Usage;
		}
		episode = static_cast<long>(*value);
	}

	if (*url == "-") return episodes_stdin(args, json, *filters, track, episode);

	ParserStore store;
	populate_store(store);
	CatalogServices services;
	apply_cached_catalog(&store, nullptr, services);
	std::optional<UrlRoute> route = store.route_url(*url);
	if (!route || route->type != GetterSuggestionType::Anime) {
		std::println(stderr, "{}: no anime source handles '{}'", program, *url);
		return Runtime;
	}
	RequestorContext ctx = services.parser_context();
	auto ready_opt = make_ready_context(ctx, *route->parser, args);
	if (!ready_opt) return Runtime;
	RequestorContext ready = std::move(*ready_opt);
	auto root = route->parser->animes_getter();
	const std::string source = route->parser->info().name;
	auto getter_result = coro::sync_wait(root->parse_url(ready, std::move(route->url)));
	if (!getter_result) {
		std::println(stderr, "{}: parse failed: {}", program, getter_result.error().message);
		return Runtime;
	}
	return emit_episodes_hierarchy(ready, **getter_result, source, *filters, track, episode, json, nullptr);
}

/// sync_wait a root getter's search_support and unwrap it; nullopt (after
/// printing) on error. All browsable domain roots share this contract.
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
	CatalogServices services;
	apply_cached_catalog(&store, nullptr, services);
	const std::shared_ptr<Parser> parser = store.find_by_key(*pkey);
	if (!parser) {
		std::println(stderr, "{}: no parser '{}' (try `{} list parsers`)", program, *pkey, program);
		return Usage;
	}

	RequestorContext ctx = services.parser_context();
	auto ready_opt = make_ready_context(ctx, *parser, args);
	if (!ready_opt) {
		return Runtime;
	}
	RequestorContext ready = std::move(*ready_opt);

	using namespace compatibilities_flags;
	const CompatibilitiesFlags pflags = parser->compatibilities().flags;
	const bool is_manga  = pflags.has(supports_manga_store);
	const bool is_anime  = pflags.has(supports_anime_store);
	const bool is_images = pflags.has(supports_images_store) || pflags.has(supports_images_search);
	if (!is_manga && !is_anime && !is_images) {
		std::println(stderr, "{}: parser '{}' has no browsable category", program, *pkey);
		return Usage;
	}
	const std::string source        = parser->info().name;
	const std::string_view category = is_manga ? "manga" : (is_anime ? "anime" : "images");

	if (which == "search") {
		std::optional<SearchCompatibilities> sc;
		if (is_manga) sc = fetch_search_support(ready, parser->mangas_getter());
		else if (is_anime) sc = fetch_search_support(ready, parser->animes_getter());
		else sc = fetch_search_support(ready, parser->images_getter());
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
	if (is_anime) {
		const AnimeGetterRootCompatibilities s = parser->animes_getter()->latest_support();
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
	if (verb == "extract") return extract(verb_args, json);
	if (verb == "episodes") return episodes(verb_args, json);
	if (verb == "download") return download(verb_args, json);
	if (verb == "catalog") return catalog(verb_args, json);

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
