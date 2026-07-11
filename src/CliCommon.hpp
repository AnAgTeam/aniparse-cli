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
#include <aniparse/types/Search.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace anip::cli {

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

} // namespace anip::cli
