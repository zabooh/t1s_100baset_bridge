# json/

Every JSON file the repo's own tooling reads or writes lives here, tracked or not -
one predictable place instead of scattered across the repo root. Read/written by
the tools in [`../scripts/`](../scripts/); nothing here is touched by the firmware
itself. Not included: `.main-meta/main.json` inside the two MPLAB X `.X` project
folders (IDE-generated project metadata, analogous to `nbproject/` - left where
MPLAB X expects it) and `setup_compiler.config` (JSON content, but a deliberately
different extension, at the repo root next to `build.bat`).

## Tracked (part of the repo)

| File | Written by | Read by | Purpose |
|---|---|---|---|
| `boards.json` | hand-edited | `flash_boards.py` | Maps a board *role* (`bridge`, `follower_a`, `follower`/B) to the EDBG probe serial that board is wired to, plus a human-readable description. The one thing it deliberately does not say is which firmware image belongs to a role - that mapping lives in `flash_boards.py`'s `ROLE_IMAGES`, so `flash_boards.py <role>` can flash "the board doing job X" without the caller knowing its probe serial. |
| `bridge_config.json` | `bridge_gui.py` | `bridge_gui.py` | **Session state only** - COM port, bridge parameters (IP/mask/gateway/DNS/MAC per interface, PLCA id/count, mirror), and the last-read LAN8651 register values. Carries no register *map* (addresses, names, bit fields) - that is `lan8651_model.json`, read-only from here. Safe to delete; the GUI recreates it with defaults. |
| `env_model.json` | hand-edited | `check_env_model.py`, `bridge_gui.py` | Model of the persistent environment stored in the SAME54's Emulated EEPROM, one entry per firmware variant's environment id + version (`showenv`'s identity line is matched against this to pick the right entry). Says which fields exist, the regex pattern each is read back with, and the `setenv` key each is written with. `check_env_model.py` cross-checks every `cli_key` against `firmware/src/env.c`'s actual `setenv` table in both directions. Mistakes get corrected **here**, never in `bridge_gui.py`. |
| `lan8651_model.json` | hand-edited | `check_register_model.py`, `bridge_gui.py` | The LAN8651 register model - 183 registers, 538 bit fields, each with its data sheet section/page, errata notes, and access/reset behaviour. `bridge_gui.py` only ever reads this (its "LAN8651 Registers" tab); it never writes it. `check_register_model.py` checks it against itself (MMS vs. group, duplicate addresses/mnemonics, malformed or overlapping bit ranges, missing names) and reports how much of it is verified against the data sheet. Mistakes get corrected **here**, then re-run the checker. |

## Gitignored (per-machine, never commit)

| File | Written by | Read by | Purpose |
|---|---|---|---|
| `bench.json` | `install_prereqs.py` (`install.bat --select`) | `flash_same54.py`, `flash_boards.py` (via `flash_same54`) | Which EDBG/nEDBG probe `flash.bat` programs by default, keyed on the probe's USB serial - meaningless on another bench where the serials differ, hence gitignored. `install.bat --select` (or a bare `install.bat`, which asks every run) rewrites it; Enter keeps the current choice. |
| `term_ports.json` | `gui_term.py` ("Setup > Configure Ports") | `gui_term.py` | Which COM port is assigned to each of `gui_term.py`'s three terminal panes (picked directly from the live port list, not resolved from a probe serial like `bench.json`), plus the shared font size and text color from the same dialog's "Display" section. Machine-specific; the dialog is the source of truth, do not hand-edit it. |

Both gitignored files degrade gracefully when missing (first run on a fresh clone):
`flash_same54.py` falls back to letting pyOCD pick the probe itself (only works
with exactly one connected), and `gui_term.py` shows all three panes unassigned
rather than failing.
