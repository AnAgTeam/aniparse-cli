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
├── src/main.cpp              the CLI itself
├── cmake/                    Findaniparse-parsers.cmake (source composition)
└── aniparse-parsers/         submodule → libaniparse/ (nested submodule)
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

| Command | Status |
|---|---|
| `list parsers` | implemented |
| `list filters` | planned |
| `support (latest\|search) -p <parser>` | planned |
| `latest -p <parser> [--from N] [--limit N] [--sort S]` | planned |
| `search -p <parser> -q <query> [--filter k=v ...]` | planned |
| `parse <url>` | planned |

`--json` switches any command to machine-readable output; `--help` prints usage.
Exit codes: `0` ok, `1` runtime error, `2` usage/validation error.
