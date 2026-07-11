# anip — the aniparse CLI

A command-line interface to the [libaniparse](https://github.com/AnAgTeam/aniparse)
library. **Not a downloader** — the point is automation over the whole library:
pipelines like `anip search --json | jq ... | anip parse --download`, or a cron
tracker over `anip latest --json`.

## Layout

The CLI is a product built on top of the neutral library and its parser set. It
carries neither by copy: `aniparse-parsers` is a git submodule, and it in turn
submodules the core `libaniparse`, so a checkout resolves a matched core+parser
pair.

```
aniparse-cli/
├── src/
│   ├── main.cpp          verb dispatch + search/latest/support/parse + shared helpers
│   ├── CliCommon.hpp     internal shared surface (namespace aniparse::cli)
│   ├── CliFilters.cpp    --filter vocabulary + `list filters` / `support` output
│   └── CliDownload.cpp   the download verb and its image/manga dumping
├── cmake/                Findaniparse-parsers.cmake + anip_extensions.hpp.in (seam A)
└── aniparse-parsers/     submodule → libaniparse/ (nested submodule)
```

## Build

```
git clone --recursive <url> aniparse-cli
cmake --preset x64-debug
cmake --build out/build/x64-debug
```

## Bring your own parsers

The public binary ships only the showcase parsers. To build your own `anip` with
extra (e.g. private) sources, point it at an extension library that exposes a
registrar `void reg(aniparse::ParserStore&)`. This is **static composition** — the
extension is compiled and linked like any other target, so there is no dynamic
loading and no ABI/RCE surface. Four cache variables drive it:

```
cmake --preset x64-debug \
  -D ANIP_EXTENSION_SUBDIRS="/path/to/my-extensions" \  # add_subdirectory'd
  -D ANIP_EXTENSION_LIBS="my-ext" \                     # target(s) to link
  -D ANIP_EXTENSION_HEADERS="myext/Register.hpp" \      # declares the registrar
  -D ANIP_EXTENSION_REGISTRARS="myext::emplace_my_parsers"
```

Each registrar is called after the default set, so your parsers join the store
alongside the showcase ones. A registrar is just:

```cpp
// myext/Register.hpp
namespace myext { void emplace_my_parsers(aniparse::ParserStore& store); }
```

Runtime plugin loading (`--extensions <dir>`) is intentionally not offered here:
loading arbitrary native code is a code-execution channel, and the C++ ABI across
a shared-library boundary is fragile. If it lands later it will be a narrow,
opt-in C entry point over this same registrar seam.

## Commands

| Command | What it does |
|---|---|
| `list parsers` | the available sources and their capabilities |
| `list filters` | the search-filter vocabulary and `--filter k=v` value syntax |
| `support (latest\|search) -p <parser>` | what a source accepts: filters, sorts, flags |
| `latest -p <parser> [--from N] [--limit N] [--sort S] [--asc]` | browse the newest items |
| `search -p <parser> [-q <query>] [--filter k=v ...] [--from N] [--limit N]` | search a source |
| `parse <url>` | route a URL to its source and fetch info |
| `download [<url>] [--dest DIR] [--chapters 1-4,8] [--jobs N]` | save files |

`search` takes `-q` and/or repeatable `--filter k=v` (`k=!v` excludes; different keys
AND across axes). `download` saves an image container or a manga's chapters→pages
(each file buffered; streaming/CBZ still to come); with **no** URL it reads the
`--json` records that `search`/`latest` emit from stdin — closing the
`anip search --json | … | anip download` pipe.

`--json` switches any command to machine-readable output; `--help` prints usage.
Exit codes: `0` ok, `1` runtime error, `2` usage/validation error.

## Authentication

Sources that need credentials are authenticated per verb (search / latest / parse /
download). Pass a token or a user+password pair — as flags, or (secret-safe) as
environment variables so nothing lands in argv or shell history:

| | flag | environment |
|---|---|---|
| token | `--token T` | `ANIP_<PARSER>_TOKEN` |
| user + password | `--user U --password P` | `ANIP_<PARSER>_USER` + `ANIP_<PARSER>_PASSWORD` |

`<PARSER>` is the parser id upper-cased. Flags win over the environment; with no
credentials the source is queried anonymously. Credentials are used for the request
only and are never written anywhere. For example Gelbooru (401 anonymous) maps
`--user` to your user_id and `--password` to your api_key:

```
export ANIP_GELBOORU_USER=123456
export ANIP_GELBOORU_PASSWORD=<api_key>
anip search -p Gelbooru -q cat_ears --json
```
