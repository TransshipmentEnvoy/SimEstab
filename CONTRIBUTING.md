# Contributing

This file holds the rules for changes to SimEstab. It covers commit subjects.

## Commit subjects

A commit subject is `<prefix>: <summary>`. The prefix is three letters and names what the
commit changes. A commit that spans several kinds takes the prefix of its main change.

| Prefix | Use |
|---|---|
| `new:` | adds a component or feature |
| `mod:` | changes existing code |
| `rfr:` | restructures code (rename, move, split) without changing behaviour |
| `rmv:` | removes code or files |
| `fix:` | fixes a bug |
| `fmt:` | formatting only (clang-format, black, isort); no behaviour change |
| `rev:` | reverts an earlier commit |
| `tst:` | adds or changes tests only |
| `exm:` | adds or changes examples |
| `dep:` | changes dependencies |
| `sys:` | changes the build system (CMake, Conan recipes and profiles, `setup_ext`, `pyproject.toml`, build scripts in `script/`) |
| `cfg:` | changes tool config (`.clang-format`, `.clang-tidy`, `.gitignore`, `pytest.ini`) or the program's run config |
| `rsc:` | changes program data: non-code files the program loads to work (shaders, data tables, scenario and mod definitions, fonts, translations) |
| `ast:` | changes art and media (images, textures, models, sounds) |
| `doc:` | changes docs only |

**An intermediate commit that does not pass the tests, or is not worth testing, is marked
`(wip)` after the prefix.** A commit without `(wip)` passes the tests.

```text
new: (wip) bundled recipe tar cache mechanism
```
