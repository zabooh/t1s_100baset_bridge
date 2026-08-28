"""Hold docs/CLI_COMMANDS.md against the firmware, mechanically.

    python cli_doc_check.py            check, exit code 0 or 1
    python cli_doc_check.py --list     just print what the firmware registers

WHY THIS EXISTS
A command reference is worth nothing the moment it drifts from the code, and drift
is silent: nobody notices a missing command, because you only look up commands you
already know exist. So the list is not maintained by hand - it is derived from the
SYS_CMD_DESCRIPTOR tables in the firmware and compared against the document.

Three checks, because a doc can fail in three ways:
  1. a command exists but has no section        -> undocumented
  2. a section exists for no command            -> stale, or a typo in the name
  3. a quick-reference link points nowhere      -> the table lies about itself

Exit code 1 on any of them, so it can go into a build or test script.
"""
import os
import re
import sys

SRC = os.path.join('firmware', 'src')
DOC = os.path.join('docs', 'CLI_COMMANDS.md')
# The project's own tables. The Harmony stack groups (tcpip, iperf) are deliberately
# not checked here: which of those exist depends on the MCC configuration, so the
# device's own `help` is the authority for them, not this repo.
FILES = ('app.c', 'env.c', 'lan865x_diag.c', 'noip_test.c', 'port_mirror.c')


def firmware_commands():
    found = {}
    for fn in FILES:
        path = os.path.join(SRC, fn)
        if not os.path.exists(path):
            print('  WARNING: %s is missing - a moved file would silently shrink this list' % path)
            continue
        txt = open(path, encoding='utf-8', errors='replace').read()
        m = re.search(r'SYS_CMD_DESCRIPTOR\s+\w*cmd_tbl\[\]\s*=\s*\{(.*?)\n\};', txt, re.S)
        if not m:
            print('  WARNING: no command table found in %s' % fn)
            continue
        for name in re.findall(r'\{\s*"([a-zA-Z_0-9]+)"', m.group(1)):
            found[name] = fn
    return found


def main():
    fw = firmware_commands()
    if '--list' in sys.argv:
        for n in sorted(fw):
            print('%-12s %s' % (n, fw[n]))
        return 0
    if not fw:
        print('FAIL: no commands found in the firmware - check the paths above')
        return 1

    doc = open(DOC, encoding='utf-8').read()
    documented = set(re.findall(r'^###\s+`([a-zA-Z_0-9]+)`', doc, re.M))
    quickref = set(re.findall(r'\[`([a-zA-Z_0-9]+)`\]\(#', doc))

    anchors = set(re.findall(r'\]\(#([a-z_0-9-]+)\)', doc))
    heads = set()
    for h in re.findall(r'^#{2,3}\s+(.+)$', doc, re.M):
        a = re.sub(r'[^a-z0-9 _-]', '', h.lower().replace('`', '')).strip().replace(' ', '-')
        heads.add(a)

    undocumented = sorted(set(fw) - documented)
    stale = sorted(documented - set(fw))
    not_listed = sorted(set(fw) - quickref)
    dead = sorted(a for a in anchors if a not in heads)

    print('firmware registers : %d commands in %d tables' % (len(fw), len(FILES)))
    print('%s documents       : %d sections, %d quick-reference rows' % (DOC, len(documented), len(quickref)))
    print()
    rc = 0
    for label, items in (('undocumented commands', undocumented),
                         ('sections without a command', stale),
                         ('missing from the quick reference', not_listed),
                         ('dead anchors', dead)):
        if items:
            print('FAIL  %-32s %s' % (label + ':', ', '.join(items)))
            rc = 1
    if rc == 0:
        print('PASS  document and firmware agree on all %d commands' % len(fw))
    return rc


if __name__ == '__main__':
    sys.exit(main())
