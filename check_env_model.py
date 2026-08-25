#!/usr/bin/env python3
"""Check env_model.json - the model of the persistent environment in the EEPROM.

Two kinds of check. The cheap ones are about the file itself: required keys, ids that
are four characters, patterns that compile and capture something, English throughout.

The one that earns its keep compares the model against the firmware: every cli_key must
be a key that env.c's setenv actually accepts, and every setenv key should appear in the
model. A model naming a key the firmware does not know produces a GUI button that fails
silently at the device; a key missing from the model is a setting nobody can reach.

    python check_env_model.py
    python check_env_model.py --quiet

Exit code 0 = no errors, 1 = at least one.
"""

import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).parent
MODEL_PATH = HERE / "env_model.json"
ENV_C = HERE / "firmware" / "src" / "env.c"

# Same conservative list as check_register_model.py - see the note there about "die".
GERMAN_WORDS = re.compile(
    r"\b(und|nicht|oder|wenn|dass|beim|muss|wird|werden|sind|auch|eine|einem|einen|"
    r"kein|keine|dieser|diese|dieses|nach|noch|schon|sowie|damit|weil|aber)\b", re.I)

REQUIRED_FIELD_KEYS = ("label", "cli_key", "type", "pattern", "description")


def firmware_setenv_keys():
    """The keys env.c's setenv accepts: the strcmp table plus the special-cased ones."""
    if not ENV_C.is_file():
        return None
    src = ENV_C.read_text(encoding="utf-8", errors="ignore")
    keys = set(re.findall(r'strcmp\(\s*key\s*,\s*"([a-z0-9_]+)"\s*\)', src))
    keys |= set(re.findall(r'strcmp\(\s*argv\[1\]\s*,\s*"([a-z0-9_]+)"\s*\)', src))
    return keys


def check(model, errors, warnings):
    for key in ("model_version", "note", "identity", "environments"):
        if key not in model:
            errors.append(f"missing top-level key: {key}")

    ident = model.get("identity", {})
    if "pattern" not in ident:
        errors.append("identity has no pattern")
    else:
        try:
            rx = re.compile(ident["pattern"])
        except re.error as exc:
            errors.append(f"identity pattern does not compile: {exc}")
        else:
            if rx.groups != len(ident.get("groups", [])):
                errors.append(f"identity: pattern captures {rx.groups} groups, "
                              f"'groups' names {len(ident.get('groups', []))}")

    fw_keys = firmware_setenv_keys()
    seen_cli = set()

    for name, env in model.get("environments", {}).items():
        where = f"environments.{name}"
        env_id = env.get("id", "")
        if len(env_id) != 4:
            errors.append(f"{where}: id {env_id!r} is not four characters - the id is the "
                          "record's magic and identifies the firmware variant")
        if not isinstance(env.get("version"), int):
            errors.append(f"{where}: version must be a number")
        if not env.get("variant"):
            errors.append(f"{where}: no variant name")
        if name != f"{env_id} v{env.get('version')}":
            warnings.append(f"{where}: key does not read as '<id> v<version>'")

        fields = env.get("fields", {})
        if not fields:
            errors.append(f"{where}: no fields")
        for fkey, f in fields.items():
            fw = f"{where}.fields.{fkey}"
            for req in REQUIRED_FIELD_KEYS:
                if not f.get(req):
                    errors.append(f"{fw}: missing {req}")
            if "pattern" in f:
                try:
                    rx = re.compile(f["pattern"])
                except re.error as exc:
                    errors.append(f"{fw}: pattern does not compile: {exc}")
                else:
                    if rx.groups < 1:
                        errors.append(f"{fw}: pattern captures nothing - it must have a group "
                                      "for the value")
            cli = f.get("cli_key")
            if cli:
                seen_cli.add(cli)
                if fw_keys is not None and cli not in fw_keys:
                    errors.append(f"{fw}: cli_key {cli!r} is not a key env.c accepts "
                                  f"(setenv would answer \"unknown key\")")

    if fw_keys is None:
        warnings.append(f"{ENV_C} not found - could not compare against the firmware")
    else:
        for missing in sorted(fw_keys - seen_cli):
            warnings.append(f"env.c accepts setenv key {missing!r}, but no field in the model "
                            "uses it - that setting is unreachable from the GUI")

    def walk(node, path):
        if isinstance(node, str):
            hit = GERMAN_WORDS.search(node)
            if hit:
                errors.append(f"{path}: looks like German ({hit.group(0)!r}) - the model is "
                              f"English throughout: {node[:60]}...")
        elif isinstance(node, dict):
            for k, v in node.items():
                walk(v, f"{path}.{k}" if path else str(k))
        elif isinstance(node, list):
            for i, v in enumerate(node):
                walk(v, f"{path}[{i}]")

    walk(model, "")


def main():
    parser = argparse.ArgumentParser(description="Check the environment model")
    parser.add_argument("model", nargs="?", default=str(MODEL_PATH))
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    path = Path(args.model)
    try:
        model = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        print(f"ERROR: {path} not found")
        return 1
    except ValueError as exc:
        print(f"ERROR: {path} is not valid JSON: {exc}")
        return 1

    errors, warnings = [], []
    check(model, errors, warnings)

    if not args.quiet:
        envs = model.get("environments", {})
        print(f"model       : {path.name} (version {model.get('model_version')})")
        for name, env in envs.items():
            print(f"environment : {name}  ({env.get('variant')}), "
                  f"{len(env.get('fields', {}))} fields, "
                  f"{env.get('source', {}).get('size_bytes', '?')} bytes on the EEPROM")
        keys = firmware_setenv_keys()
        print(f"firmware    : {ENV_C.name} accepts {len(keys) if keys else '?'} setenv keys")
        print()

    for w in warnings:
        print(f"WARNING  {w}")
    for e in errors:
        print(f"ERROR    {e}")

    if errors:
        print(f"\n{len(errors)} error(s), {len(warnings)} warning(s)")
        return 1
    print(f"\nno errors ({len(warnings)} warning(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
