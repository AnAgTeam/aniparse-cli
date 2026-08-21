/*
 * Copyright (C) 2025-2026 Toilettrauma
 *
 * Internal shared surface for the anip CLI, split across translation units:
 *   main.cpp          verb dispatch + the search/latest/support/parse verbs and
 *                     the low-level helpers defined here (flag_value, parse_uint, …)
 *   CliFilters.cpp    the search-filter vocabulary: --filter parsing and the
 *                     `list filters` / `support` presentation
 *   CliDownload.cpp   the download verb and its image/manga dumping machinery
 *
 * Everything lives in namespace anip::cli. This header declares only the members
 * used across those TUs; each unit keeps its own helpers file-local.
 */
#pragma once
#include <aniparse/ParserStore.hpp>
#include <aniparse/ClientContext.hpp>
#include <aniparse/types/Search.hpp>
#include <aniparse/types/Authentication.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aniparse {
class VideoExtractorStore;
}

namespace aniparse::cli {

inline constexpr std::string_view program = "anip";

// 0 ok, 1 runtime error, 2 usage/validation error. --help exits 0.
enum ExitCode : int { Ok = 0, Runtime = 1, Usage = 2 };

// --- Shared low-level helpers (defined in main.cpp) ---

/// The value token following @p name (e.g. "-p"), or nullopt if @p name is absent
/// or has no following token.
std::optional<std::string_view> flag_value(std::span<const std::string_view> args,
                                           std::string_view name);

/// Parse a non-negative integer that consumes the whole token; nullopt otherwise.
std::optional<long long> parse_uint(std::string_view s);

std::string join(const std::vector<std::string_view>& parts, std::string_view sep);

/// Decode the capability bitfield into short human labels for a listing row.
std::vector<std::string_view> capability_labels(const aniparse::CompatibilitiesFlags& f);

/// Build a store: the public showcase parsers plus any extension parser sets
/// linked into this build (seam A — see cmake/anip_extensions.hpp.in).
void populate_store(aniparse::ParserStore& store);

// --- Volatile catalog (defined in CliCatalog.cpp) ---

/// The shared service bundle for one verb run: an AsyncClient plus the swappable
/// holders a catalog apply feeds (mirrors, selectors and resources on the
/// parser-facing state; a separate mirror holder on the extractor-facing one).
/// Build contexts from it only AFTER apply_cached_catalog() — a context
/// snapshots the mirror source at construction.
struct CatalogServices {
	std::shared_ptr<aniparse::ServiceState> parsers;
	std::shared_ptr<aniparse::ServiceState> extractors;

	CatalogServices();
	aniparse::RequestorContext parser_context() const;
	aniparse::RequestorContext extractor_context() const;
};

/// Apply the cached volatile catalog (left by `catalog apply`, or at the
/// ANIP_CATALOG path) to the given stores and the service holders. Either store
/// may be null. No-op when no cache file exists; a rejected cache warns and the
/// run continues on static domains. Signature verification is stubbed — dev tool.
void apply_cached_catalog(aniparse::ParserStore* parser_store,
                          aniparse::VideoExtractorStore* extractor_store,
                          const CatalogServices& services);

int catalog(std::span<const std::string_view> args, bool json);

// --- Authentication (defined in main.cpp) ---

/// Resolve credentials for @p parser_id from the shared auth flags, falling back to
/// environment variables so secrets need not appear in argv / shell history:
///   --token T            or  ANIP_<PARSER>_TOKEN            -> AuthenticationToken
///   --user U --password P or  ANIP_<PARSER>_USER/_PASSWORD  -> AuthenticationUserPassword
/// where <PARSER> is the parser identifier upper-cased (e.g. ANIP_GELBOORU_USER).
/// Flags win over the environment. nullopt = no credentials supplied (run anonymous);
/// a lone half of a user/password pair is reported as an error via the returned flag.
std::optional<aniparse::AuthenticationData> resolve_credentials(
    std::string_view parser_id, std::span<const std::string_view> args);

/// Derive @p parser's config from @p base and, when credentials are supplied,
/// authenticate it — returning the RequestorContext every getter then runs on.
/// The single seam that folds make_config + optional authenticate_context. Prints
/// and returns nullopt only when an actual authentication attempt fails; with no
/// credentials it returns the (anonymous) derived context.
std::optional<aniparse::RequestorContext> make_ready_context(
    const aniparse::RequestorContext& base, aniparse::Parser& parser,
    std::span<const std::string_view> args);

// --- Filter vocabulary (defined in cli_filters.cpp) ---

/// Build structured search filters from repeated `--filter k=v` (v may be `!x` to
/// exclude), typed against the source's declared @p supported table. Prints and
/// returns nullopt on an unknown key or malformed value.
std::optional<aniparse::SearchItems> build_search_filters(
    std::span<const std::string_view> args, const aniparse::SearchItems& supported);

int list_filters(bool json);

int print_search_support(std::string_view source, std::string_view category,
                         const aniparse::SearchCompatibilities& sc, bool json);

int print_latest_support(std::string_view source, std::string_view category,
                         const aniparse::SupportedSorts& sorts,
                         const aniparse::CompatibilitiesFlags& flags, bool json);

// --- Download verb (defined in cli_download.cpp) ---

int download(std::span<const std::string_view> args, bool json);

} // namespace aniparse::cli
