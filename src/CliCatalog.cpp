/*
 * Copyright (C) 2025-2026 Toilettrauma
 *
 * Author: Toilettrauma <macosinternal@gmail.com>
 *
 * The `catalog` verb and the cached-catalog startup hook: a dev harness for the
 * signed volatile catalog. `catalog apply <file>` validates a payload and leaves
 * it in a cache file next to the binary (or at ANIP_CATALOG); every verb then
 * applies that cache to its stores and service holders before building contexts.
 */
#include "CliCommon.hpp"

#include <aniparse/catalog/CatalogManager.hpp>
#include <aniparse/net/Client.hpp>
#include <aniparse/video/VideoExtractor.hpp>

#include <boost/json.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <print>
#include <string>
#include <string_view>

#ifdef __APPLE__
#	include <mach-o/dyld.h>
#endif

namespace aniparse::cli {
using namespace aniparse;

namespace {

// Dev seam: the CLI has no pinned key, so signature verification accepts
// everything. The verify -> decode -> apply pipeline still runs end to end;
// only the crypto is stubbed. Never reuse this verifier outside the dev CLI.
struct DevVerifier final : SignatureVerifier {
	bool verify(std::string_view, std::string_view) const override { return true; }
};

/// The cache file `catalog apply` writes and every verb reads back: ANIP_CATALOG
/// when set, otherwise anip-catalog.json next to the executable (falling back to
/// the current directory when the executable path cannot be resolved).
std::filesystem::path cache_path() {
#ifdef _MSC_VER
#	pragma warning(push)
#	pragma warning(disable : 4996) // std::getenv is the portable read; no writes, safe here
#endif
	if (const char* env = std::getenv("ANIP_CATALOG"); env && *env) {
		return env;
	}
#ifdef _MSC_VER
#	pragma warning(pop)
#endif
#ifdef __APPLE__
	uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::string buffer(size, '\0');
	if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
		return std::filesystem::path(buffer.c_str()).parent_path() / "anip-catalog.json";
	}
#endif
	return std::filesystem::current_path() / "anip-catalog.json";
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return std::nullopt;
	}
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{});
}

std::string_view error_name(CatalogError error) {
	switch (error) {
	case CatalogError::BadSignature:       return "bad signature";
	case CatalogError::BadFormat:          return "bad format";
	case CatalogError::UnsupportedVersion: return "unsupported schema version";
	case CatalogError::StaleRevision:      return "stale revision";
	}
	return "unknown error";
}

boost::json::object section_json(
    const std::map<std::string, std::vector<std::string>, std::less<>>& domains,
    const std::map<std::string, std::vector<std::string>, std::less<>>& mirrors) {
	auto urls_json = [](const std::vector<std::string>& urls) {
		boost::json::array out;
		for (const std::string& url : urls) {
			out.emplace_back(url);
		}
		return out;
	};
	boost::json::object out;
	auto put_list = [&urls_json, &out](std::string_view id, std::string_view field,
	                                   const std::vector<std::string>& list) {
		boost::json::object* entry = nullptr;
		if (boost::json::value* existing = out.if_contains(id)) {
			entry = &existing->as_object();
		} else {
			entry = &out.emplace(id, boost::json::object{}).first->value().as_object();
		}
		(*entry)[field] = urls_json(list);
	};
	for (const auto& [id, list] : domains) {
		put_list(id, "domains", list);
	}
	for (const auto& [id, list] : mirrors) {
		put_list(id, "mirrors", list);
	}
	return out;
}

boost::json::object summary_json(const CatalogData& data) {
	boost::json::object out;
	out["revision"]   = data.revision;
	out["parsers"]    = section_json(data.domains, data.mirrors);
	out["extractors"] = section_json(data.extractor_domains, data.extractor_mirrors);
	boost::json::object selectors;
	for (const auto& [name, css] : data.selectors) {
		selectors[name] = css;
	}
	out["selectors"] = std::move(selectors);
	return out;
}

void print_section(std::string_view title, std::string_view indent,
                   const std::map<std::string, std::vector<std::string>, std::less<>>& domains,
                   const std::map<std::string, std::vector<std::string>, std::less<>>& mirrors) {
	std::println("{}: {} domain override(s), {} mirror override(s)",
	    title, domains.size(), mirrors.size());
	auto print_entries = [&](const auto& map, std::string_view label) {
		for (const auto& [id, urls] : map) {
			std::println("{}  {} {}: {}", indent, id, label, join(
			    std::vector<std::string_view>(urls.begin(), urls.end()), ", "));
		}
	};
	print_entries(domains, "domains");
	print_entries(mirrors, "mirrors");
}

void print_summary(const CatalogData& data) {
	std::println("revision {}", data.revision);
	print_section("parsers", "", data.domains, data.mirrors);
	print_section("extractors", "", data.extractor_domains, data.extractor_mirrors);
	std::println("selectors: {} override(s)", data.selectors.size());
}

/// Decode a payload through the dev verifier; prints the reason and returns
/// nullopt when the payload does not survive verify + decode.
std::optional<CatalogData> decode_or_report(const std::string& payload) {
	DevVerifier verifier;
	auto decoded = decode_catalog(payload, "dev-stub", verifier);
	if (!decoded) {
		std::println(stderr, "{}: catalog rejected: {}", program, error_name(decoded.error()));
		return std::nullopt;
	}
	return std::move(*decoded);
}

} // namespace

CatalogServices::CatalogServices() {
	auto client = std::make_shared<AsyncClient>();
	// Parser-facing state: the full holder set (mirrors, selectors, resources).
	parsers = std::make_shared<ServiceState>();
	parsers->client = client;
	parsers->mirrors = std::make_shared<MirrorSourceHolder>();
	parsers->selectors = std::make_shared<html::SelectorSourceHolder>();
	parsers->resources = std::make_shared<ResourceCache>();
	// Extractor-facing state: a separate mirror holder, so catalog overrides
	// keyed by extractor identifier never mix with the parser ones.
	extractors = std::make_shared<ServiceState>();
	extractors->client = client;
	extractors->mirrors = std::make_shared<MirrorSourceHolder>();
}

RequestorContext CatalogServices::parser_context() const {
	return RequestorContext(parsers, nullptr);
}

RequestorContext CatalogServices::extractor_context() const {
	return RequestorContext(extractors, nullptr);
}

void apply_cached_catalog(ParserStore* parser_store, VideoExtractorStore* extractor_store,
                          const CatalogServices& services) {
	const std::filesystem::path path = cache_path();
	const std::optional<std::string> payload = read_file(path);
	if (!payload) {
		return; // no cached catalog — static domains and built-in mirrors only
	}
	DevVerifier verifier;
	// CatalogManager is built around a ParserStore; when a verb only wants the
	// extractor half, a throwaway store absorbs the parser one.
	ParserStore throwaway;
	CatalogManager manager(parser_store ? *parser_store : throwaway, verifier,
	    services.parsers, extractor_store, services.extractors);
	auto applied = manager.apply(*payload, "dev-stub");
	if (!applied) {
		std::println(stderr, "{}: cached catalog '{}' ignored: {}",
		    program, path.string(), error_name(applied.error()));
		return;
	}
	std::println(stderr, "{}: applied cached catalog revision {} from '{}'",
	    program, *applied, path.string());
}

int catalog(std::span<const std::string_view> args, bool json) {
	if (args.empty()) {
		std::println(stderr, "{}: catalog needs a subcommand (apply <file>|status|clear)", program);
		return Usage;
	}
	const std::filesystem::path path = cache_path();

	if (args[0] == "apply") {
		if (args.size() < 2) {
			std::println(stderr, "{}: catalog apply needs a catalog <file>", program);
			return Usage;
		}
		const std::optional<std::string> payload = read_file(args[1]);
		if (!payload) {
			std::println(stderr, "{}: cannot read catalog file '{}'", program, args[1]);
			return Runtime;
		}
		// Validate through the same verify+decode path the startup hook uses
		// before anything is cached.
		const std::optional<CatalogData> data = decode_or_report(*payload);
		if (!data) {
			return Runtime;
		}
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out) {
			std::println(stderr, "{}: cannot write catalog cache '{}'", program, path.string());
			return Runtime;
		}
		out << *payload;
		out.close();
		std::println(stderr, "{}: cached catalog at '{}' (signature check stubbed, dev only)",
		    program, path.string());
		if (json) {
			std::println("{}", boost::json::serialize(summary_json(*data)));
		} else {
			print_summary(*data);
		}
		return Ok;
	}

	if (args[0] == "status") {
		const std::optional<std::string> payload = read_file(path);
		if (!payload) {
			std::println(stderr, "{}: no cached catalog (expected at '{}')", program, path.string());
			return Ok;
		}
		const std::optional<CatalogData> data = decode_or_report(*payload);
		if (!data) {
			return Runtime;
		}
		if (json) {
			std::println("{}", boost::json::serialize(summary_json(*data)));
		} else {
			std::println("cached at '{}'", path.string());
			print_summary(*data);
		}
		return Ok;
	}

	if (args[0] == "clear") {
		std::error_code error;
		const bool removed = std::filesystem::remove(path, error);
		if (error) {
			std::println(stderr, "{}: cannot remove '{}': {}", program, path.string(), error.message());
			return Runtime;
		}
		std::println(stderr, "{}: {}", program,
		    removed ? "catalog cache cleared" : "no cached catalog to clear");
		return Ok;
	}

	std::println(stderr, "{}: unknown catalog subcommand '{}'", program, args[0]);
	return Usage;
}

} // namespace aniparse::cli
