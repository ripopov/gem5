# AGENTS.md

Guidance for Codex and other AI agents working in this gem5 repository.

## Project overview
- gem5 is a modular computer-system architecture simulator (C++ and Python).
- Build system: SCons (see `SConstruct`, `site_scons/`).
- Tests live under `tests/` and `ext/testlib/`.

## Repository map
- `src/`: core simulator source (C++ and Python bindings).
- `configs/`: example simulation configuration scripts.
- `tests/`: regression and system-level tests.
- `ext/`: external packages used by the build.
- `util/`: utilities and helper scripts.
- `docs/`: documentation sources.

## Build commands
- Full optimized build (all ISAs): `scons build/ALL/gem5.opt`
- Unit tests binary: `scons build/ALL/unittests.opt`
- NULL ISA unit tests: `scons build/NULL/unittests.opt`

## Test commands
- Quick system tests (from `tests/`): `./main.py run`
- Python unit tests (after `gem5.opt` build): `./build/ALL/gem5.opt tests/run_pyunit.py`
- Run a single C++ test binary: `./build/ALL/base/bitunion.test.opt`
- List gtest cases: `./build/ALL/base/bitunion.test.opt --gtest_list_tests`

## Style and formatting
- C/C++: 4-space indents, no tabs, 79-char lines, brace style per
  `CONTRIBUTING.md`. Keep to existing conventions in touched files.
- Python: format with `black` and follow PEP 8 naming; match local style.

## Branching and commits
- Upstream development targets `develop`; avoid making changes directly on
  `stable`. Use feature branches.
- Commit headers use component tags from `MAINTAINERS.yaml` and keep the first
  line within 65 characters; body lines within 72 characters.

## Agent workflow tips
- Prefer small, focused edits and avoid touching unrelated files.
- Run the smallest relevant tests; note when tests are not run.
- If instructions conflict, defer to repo docs like `CONTRIBUTING.md` and
  `TESTING.md`.
