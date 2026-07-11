/*
 * Copyright (C) 2025-2026 Toilettrauma
 *
 * The search-filter vocabulary: parsing repeated `--filter k=v` against a source's
 * declared support table, and the human/JSON presentation of that vocabulary for
 * `list filters` and `support (search|latest)`. Split out of main.cpp.
 */
#include "CliCommon.hpp"

#include <boost/json.hpp>

#include <format>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace aniparse::cli {
using namespace aniparse;

namespace {

/// Parse an int filter value: "N" (exact), "A-B" (range), "A-" (min), "-B" (max).
std::optional<IntInterval> parse_int_interval(std::string_view v, bool exclusive) {
	IntInterval interval;
	interval.exclusive = exclusive;
	auto to_num = [](std::string_view s) -> std::optional<std::ptrdiff_t> {
		const auto n = parse_uint(s);
		return n ? std::optional<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(*n)) : std::nullopt;
	};
	const std::size_t dash = v.find('-');
	if (dash == std::string_view::npos) {
		const auto n = to_num(v);
		if (!n) {
			return std::nullopt;
		}
		interval.from = *n;
		interval.to   = *n;
		return interval;
	}
	const std::string_view lo = v.substr(0, dash);
	const std::string_view hi = v.substr(dash + 1);
	if (lo.empty() && hi.empty()) {
		return std::nullopt;
	}
	if (!lo.empty()) {
		const auto n = to_num(lo);
		if (!n) {
			return std::nullopt;
		}
		interval.from = *n;
	}
	if (!hi.empty()) {
		const auto n = to_num(hi);
		if (!n) {
			return std::nullopt;
		}
		interval.to = *n;
	}
	return interval;
}

/// The declared type of one filter as a short token — shared by the human and JSON
/// `support search` output so they never drift.
std::string_view filter_type_name(const SearchItemVariant& value) {
	if (std::holds_alternative<ItemSelection>(value))         { return "selection"; }
	if (std::holds_alternative<TextQuery>(value))             { return "text"; }
	if (std::holds_alternative<IntInterval>(value))           { return "int"; }
	if (std::holds_alternative<Checkmark>(value))             { return "flag"; }
	if (std::holds_alternative<TimeInterval>(value))          { return "time"; }
	if (std::holds_alternative<RelativeTimeInterval>(value))  { return "rel-time"; }
	return "unknown";
}

/// Up to @p max "label(token)" samples from an enumerated selection, then a
/// "… (N total)" tail. The value the user types is the token (the map key); the
/// label is what the source shows for it.
std::string selection_sample(const ItemSelection& selection, std::size_t max = 6) {
	std::string out;
	std::size_t i = 0;
	for (const auto& [token, value] : selection) {
		if (i >= max) {
			out += std::format(" … ({} total)", selection.size());
			break;
		}
		if (i) {
			out += ", ";
		}
		out += value.name.empty() ? std::string(token) : std::format("{}({})", value.name, token);
		++i;
	}
	return out;
}

/// The SOURCE-SPECIFIC detail for one filter — what `list filters` cannot show: the
/// options to choose from for an enumerated selection, or that it is open-vocabulary
/// / free text. The value-writing syntax by type lives in `list filters`, so it is
/// deliberately NOT repeated here.
std::string filter_detail(const SearchItemVariant& value) {
	if (const auto* selection = std::get_if<ItemSelection>(&value)) {
		if (selection->empty()) {
			return "open vocabulary — any token";
		}
		return std::format("from: {}", selection_sample(*selection));
	}
	if (std::holds_alternative<TextQuery>(value)) {
		return "any text";
	}
	return {}; // int / flag / time: the type name + `list filters` syntax is enough
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

} // namespace

std::optional<SearchItems> build_search_filters(std::span<const std::string_view> args,
                                                const SearchItems& supported) {
	SearchItems out;
	for (std::size_t i = 0; i + 1 < args.size(); ++i) {
		if (args[i] != "--filter") {
			continue;
		}
		const std::string_view kv = args[i + 1];
		const std::size_t eq = kv.find('=');
		if (eq == std::string_view::npos) {
			std::println(stderr, "{}: --filter expects k=v (got '{}')", program, kv);
			return std::nullopt;
		}
		const std::string key = std::string(kv.substr(0, eq));
		std::string_view    val = kv.substr(eq + 1);
		bool exclusive = false;
		if (!val.empty() && val.front() == '!') {
			exclusive = true;
			val.remove_prefix(1);
		}

		const auto decl = supported.find(key);
		if (decl == supported.end()) {
			std::println(stderr, "{}: unknown filter '{}' for this source (try `{} support search -p <parser>`)",
			             program, key, program);
			return std::nullopt;
		}

		const SearchItemVariant& type = decl->second;
		if (std::holds_alternative<TextQuery>(type)) {
			out[key] = TextQuery{ .text = std::string(val), .exclusive = exclusive };
		} else if (std::holds_alternative<ItemSelection>(type)) {
			// Just accumulate the token; membership (for enumerated axes) and exclusion
			// support are validated centrally by validate_query in run_search.
			auto it = out.find(key);
			if (it == out.end()) {
				it = out.emplace(key, ItemSelection{}).first;
			}
			std::get<ItemSelection>(it->second)[std::string(val)] =
			    ItemSelectionValue{ .name = std::string(val), .exclusive = exclusive };
		} else if (std::holds_alternative<IntInterval>(type)) {
			const auto interval = parse_int_interval(val, exclusive);
			if (!interval) {
				std::println(stderr, "{}: --filter {} expects a number or range a-b", program, key);
				return std::nullopt;
			}
			out[key] = *interval;
		} else if (std::holds_alternative<Checkmark>(type)) {
			out[key] = Checkmark{ .exclusive = exclusive };
		} else {
			std::println(stderr, "{}: --filter {} has a type this CLI can't express yet", program, key);
			return std::nullopt;
		}
	}
	return out;
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

	std::println("\n--filter k=v value syntax (a source declares each key's type):");
	std::println("  ItemSelection  k=token     repeatable = AND    e.g. --filter tag=a --filter tag=b");
	std::println("  TextQuery      k=text                          e.g. --filter title=naruto");
	std::println("  IntInterval    k=N | A-B | A- | -B             e.g. --filter icount=20-50");
	std::println("  Checkmark      k=1          (any value enables the flag)");
	std::println("  exclude (NOT)  prefix the value with '!'       e.g. --filter tag=!guro");
	std::println("  combine        different keys = AND across axes; needs -q or --filter");

	std::println("\nWhich keys (and types) a source accepts: {} support search -p <parser>", program);
	return Ok;
}

int print_search_support(std::string_view source, std::string_view category,
                         const SearchCompatibilities& sc, bool json) {
	if (json) {
		boost::json::array filters;
		for (const auto& [key, value] : sc.supported_filters) {
			boost::json::object f;
			f["key"]  = key;
			f["type"] = std::string(filter_type_name(value));
			// Enumerated options only: an empty selection is open-vocabulary and has
			// none to list. Token is what --filter takes; label is the display name.
			if (const auto* selection = std::get_if<ItemSelection>(&value); selection && !selection->empty()) {
				boost::json::array options;
				for (const auto& [token, opt] : *selection) {
					boost::json::object o;
					o["token"] = token;
					o["label"] = opt.name;
					options.push_back(std::move(o));
				}
				f["options"] = std::move(options);
			}
			filters.push_back(std::move(f));
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
		std::println("Filters: (prefix a value with '!' to exclude; different keys = AND; "
		             "value syntax: {} list filters)", program);
		for (const auto& [key, value] : sc.supported_filters) {
			std::println("  {:<10} {:<9} {}", key, filter_type_name(value), filter_detail(value));
		}
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

} // namespace aniparse::cli
