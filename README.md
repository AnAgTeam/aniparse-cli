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
│   ├── main.cpp          verb dispatch + search/latest/support/parse/episodes + shared helpers
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
| `latest -p <parser> [--from N] [--limit N] [--sort S] [--asc]` | browse the newest manga, anime, or images |
| `search -p <parser> [-q <query>] [--filter k=v ...] [--from N] [--limit N]` | search a source catalogue |
| `parse <url>` / `parse -` | fetch a routed URL, or full info for search/latest JSON read from stdin |
| `episodes <url> [--track ID] [--episode N] [--limit N]` | inspect an anime's tracks, episodes, or advertised playback sources |
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

## Examples

```sh
# discover what is available, and what a source accepts
anip list parsers
anip list filters
anip support search -p AniList

# search a metadata source, cap the result count
anip search -p AniList -q "chainsaw man" --limit 5
anip search -p Kitsu -q berserk --sort rating --limit 10

# structured filters: repeatable --filter is AND across keys; k=!v excludes.
# AniList advertises a `genres` selection (Action, Drama, Ecchi, Fantasy, …):
anip search -p AniList --filter genres=Action --filter genres=Drama --filter genres=!Ecchi \
  --sort popularity --limit 20

# browse the newest items, sorted (default order is descending; --asc flips it)
anip latest -p AniList --limit 10

# route any URL to whichever source owns it
anip parse "https://anilist.co/manga/30002"

# turn opaque search handles directly into full cards
anip --json search -p AniList -q "chainsaw man" --limit 2 | anip parse -

# inspect an anime source's playback hierarchy (metadata only: no media download)
anip episodes "<anime-url>"                         # available tracks, when the source has them
anip episodes "<anime-url>" --track 123 --limit 10   # episodes on one track
anip episodes "<anime-url>" --track 123 --episode 1  # advertised player/source for episode 1

# machine-readable output, post-processed with jq (metadata, no download)
anip --json search -p AniList -q naruto --limit 3 | jq -r '.[].title'

# the search|download pipe — find, then save every result (no URL: read stdin).
# search --json emits one {parser, handle} record per hit:
anip --json search -p Danbooru -q landscape --limit 2
# [{"offset":0,"title":"…","parser":"Danbooru","handle":{"url":"…","params":{}}}, …]
# download rebuilds each getter from those handles (from_serialized) and saves it:
anip --json search -p Danbooru -q landscape --limit 5 | anip download --dest ./out --jobs 8

# the handle is opaque — stash the JSON, filter it later with jq, then download:
anip --json search -p Danbooru -q scenery --limit 50 > feed.json
jq '[.[] | select(.title | test("mountain"))]' feed.json | anip download --dest ./out

# download a single post URL directly; --chapters selects ranges for manga sources
anip download "https://danbooru.donmai.us/posts/1234567" --dest ./out --jobs 8
anip download "<manga-url>" --chapters 1-4,8 --dest ./out

# an authenticated source — env keeps the key out of argv, then pipe into download
export ANIP_GELBOORU_USER=123456 ANIP_GELBOORU_PASSWORD=<api_key>
anip --json search -p Gelbooru -q cat_ears --limit 10 | anip download --dest ./gel --jobs 4
```
