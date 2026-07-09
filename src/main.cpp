/*
 * Copyright (C) 2025-2026 Toilettrauma
 *
 * anip — a command-line interface to libaniparse. Not a downloader: the point is
 * automation over the whole library (search --json | jq | parse --download,
 * cron trackers on latest --json). This first slice implements `list parsers`
 * end to end; the networked verbs are wired into the dispatch but not yet filled.
 */
#include <aniparse/ParserStore.hpp>
#include <aniparse/parsers/DefaultParsers.hpp>

#include <boost/json.hpp>

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
	    "  latest -p <parser> [--from N] [--limit N] [--sort S]\n"
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

int list_parsers(bool json) {
	ParserStore store;
	parsers::emplace_default_parsers(store);
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
	if (verb == "latest")  return not_yet("latest");
	if (verb == "search")  return not_yet("search");
	if (verb == "parse")   return not_yet("parse");

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
