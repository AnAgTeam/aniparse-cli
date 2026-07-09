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
