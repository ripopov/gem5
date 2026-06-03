# Agent Instructions for gem5

## Scope

These instructions apply to the entire repository.

## Repository Overview

gem5 is a mixed C++, Python, and SCons codebase for computer-system
architecture simulation. The primary source tree is under `src/`, example
configuration scripts are under `configs/`, regression and system tests are
under `tests/`, build logic is in `SConstruct` and `site_scons/`, and utility
programs are under `util/`.

## General Workflow

- Inspect the existing implementation and nearby tests before changing code.
- Keep changes focused on the requested behavior; avoid unrelated refactors.
- Do not modify generated build outputs under `build/` unless explicitly
  requested.
- Preserve user changes already present in the worktree.
- Prefer `rg`/`rg --files` for searching.

## Build Commands

- Build the primary simulator target:
  `scons build/RISCV/gem5.opt`
- Use RISCV-only build targets by default. Do not build `ALL` unless the user
  explicitly requests it or a non-RISCV change makes it necessary.
- CHI support is still required; preserve CHI-related code paths and run
  CHI-focused checks when a change touches CHI or Ruby protocol behavior.
- Build unit tests:
  `scons build/NULL/unittests.opt`
- Run the main test harness from `tests/`:
  `./main.py run`
- The full test harness can take hours. Use targeted tests or `-j` when
  appropriate, for example `python main.py run -j6`.

## Formatting and Style

- C/C++:
  - Keep lines at or below 79 characters.
  - Use 4-space indentation and no tabs.
  - Avoid trailing whitespace.
  - Match gem5 naming conventions in nearby code.
  - Function declaration return types are normally placed on their own line.
  - Opening braces for function definitions are normally placed on their own
    line; control-flow braces stay on the same line as the condition.
- Python:
  - Format with Black using the repository setting of 79 columns.
  - Follow PEP 8 naming unless nearby gem5 code has a stronger local pattern.
  - Use the repository isort grouping from `pyproject.toml`.
- When in doubt, follow the style of the file being edited.

## Tests and Verification

- Run the smallest relevant test or build target that exercises the change.
- For C++ simulator changes, prefer `scons build/RISCV/gem5.opt` or
  `build/NULL/unittests.opt` when unit coverage is relevant.
- For CHI or Ruby protocol changes, include the closest CHI-focused test or
  build target available in the repo.
- For Python changes, run Black on modified Python files and use the closest
  available targeted test.
- If a requested verification step is too expensive or unavailable, report
  what was run and why broader validation was skipped.

## Commit Message Style

If asked to prepare a commit message, follow gem5 conventions:

- Header format is `tag: short description`.
- Header must be 65 characters or fewer.
- Body lines must be 72 characters or fewer.
- Use tags appropriate to the changed component, following `MAINTAINERS.yaml`
  when possible.
