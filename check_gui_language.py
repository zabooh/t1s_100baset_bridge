#!/usr/bin/env python3
"""Check that everything bridge_gui.py puts on screen is English.

Scans string literals through the syntax tree rather than the file text, so source
comments are not part of the check - they explain the code to whoever edits it, while
these strings are what a user reads. Docstrings are skipped for the same reason.

    python check_gui_language.py [file ...]

Exit code 0 = clean, 1 = at least one German string.
"""

import ast
import re
import sys
from pathlib import Path

DEFAULT_FILES = [Path(__file__).parent / "bridge_gui.py"]

# Function words first, as in the model checkers - "die" stays out, a data sheet says "die".
#
# The second half is the lesson from the first sweep: the function words alone found 34 of
# 39 German strings. The five they missed were short status lines built from nouns and
# participles ("Registerwerte gespeichert", "Verbindung verloren", "ohne Antwort") - exactly
# the kind of text that reaches a user most often. So the nouns are in the list too.
#
# 2026-08-26 — two strings slipped through this checker for a while before someone noticed
# them on screen, both because their German words simply were not in the list yet:
# model_source_line()'s f-string ("Modell: ... Kapitel ... davon gegen das Dokument
# abgeglichen") and a log line ("> showenv (Kontrolle)"). Added below; "Register" and
# "Errata" stayed out on purpose - both are also English words, and flagging them would
# make the checker fire on correct English text (e.g. "11 registers", "Errata DS...").
GERMAN_WORDS = re.compile(
    r"\b(und|nicht|oder|wenn|dass|beim|muss|wird|werden|sind|auch|eine|einem|einen|"
    r"kein|keine|dieser|diese|dieses|noch|schon|sowie|damit|weil|aber|nur|vom|zum|zur|"
    r"ueber|über|fuer|für|waehrend|während|ohne|erst|bitte|"
    r"gedeutet|geladen|gespeichert|verbunden|gefunden|verloren|fehlt|fehlen|unlesbar|"
    r"Meldung|Fehler|Wert|Werte|Geraet|Gerät|Verbindung|Kommando|Antwort|Datei|Kennung|"
    r"Datensatz|Sonde|Achtung|Hinweis|druecken|drücken|"
    r"Modell|Kapitel|abgeglichen|davon|Dokument|Kontrolle)\b", re.I)


def gather_strings(tree):
    """Every string constant that is not a docstring, with its line number."""
    docstrings = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
            body = getattr(node, "body", [])
            if body and isinstance(body[0], ast.Expr) and isinstance(body[0].value, ast.Constant) \
                    and isinstance(body[0].value.value, str):
                docstrings.add(id(body[0].value))
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str) \
                and id(node) not in docstrings:
            yield node.lineno, node.value


def main():
    files = [Path(a) for a in sys.argv[1:]] or DEFAULT_FILES
    findings = []
    checked = 0
    for path in files:
        try:
            src = path.read_text(encoding="utf-8")
        except OSError as exc:
            print(f"ERROR: cannot read {path}: {exc}")
            return 1
        tree = ast.parse(src, filename=str(path))
        for lineno, text in gather_strings(tree):
            checked += 1
            hit = GERMAN_WORDS.search(text)
            if hit:
                findings.append((path.name, lineno, hit.group(0), text))

    print(f"checked {checked} string literals in {len(files)} file(s)")
    for name, lineno, word, text in findings:
        snippet = text.replace("\n", " ")[:70]
        print(f"ERROR    {name}:{lineno}: German ({word!r}): {snippet}")

    if findings:
        print(f"\n{len(findings)} German string(s) - the GUI speaks English")
        return 1
    print("\nno German strings")
    return 0


if __name__ == "__main__":
    sys.exit(main())
