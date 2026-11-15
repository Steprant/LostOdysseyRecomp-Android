#!/usr/bin/env python3
"""Steprant 2.1.1: can CR, XER and the lwarx reservation become locals?

XenonRecomp's cr_as_local, xer_as_local and reserved_as_local give every
generated function its own zero-initialised copy instead of the shared
ctx field. That is exact for code that writes the field before reading it in
the same function, which is what compiled code does: a compare before a
branch, an add-carrying before the add-extended, an lwarx before its stwcx.
It is not exact for a function that reads the value a caller (or a tail-call
predecessor) left behind.

This scans the generated sources (built with those options off) and lists
the functions whose first use of a field, in source order, is a read. Source
order is not execution order, so the list is conservative: it can name
functions that are fine, never miss one whose entry path reads first.

Benign reads that are not listed:
- mfcr saving the CR fields in a prologue (`rN.u64 |= ctx.crK.xx ? ... : 0`):
  with locals it saves and later restores the function's own copy, and the
  caller's copy is untouched, as the ABI requires for cr2-cr4;
- the summary-overflow copy every compare takes from XER (`, ctx.xer)`): with
  a local it is 0, as it is everywhere unless an overflow-recording
  instruction set it.

Only function names, field names and line offsets are printed, no code.
"""
import argparse
import pathlib
import re
import sys

FUNCTION = re.compile(r"^PPC_FUNC_IMPL\(__imp__(\w+)\) \{")
CR = re.compile(r"ctx\.cr([0-7])(?:\.(lt|gt|eq|so))?")
MFCR = re.compile(r"\.u64 \|?= ctx\.cr[0-7]\.(?:lt|gt|eq|so) \? 0x")
XER = re.compile(r"ctx\.xer(?:\.(ca|so|ov))?")
RESERVED = re.compile(r"ctx\.reserved(?:\.\w+)?")


def is_assignment(line, end):
    rest = line[end:].lstrip()
    return rest.startswith("=") and not rest.startswith("==")


def classify_line(line):
    """Yield (field, kind) in source order; kind is 'write', 'read' or 'benign'."""
    events = []
    if MFCR.search(line):
        for match in CR.finditer(line):
            events.append((match.start(), f"cr{match.group(1)}", "benign"))
    else:
        for match in CR.finditer(line):
            field = f"cr{match.group(1)}"
            tail = line[match.end():]
            if tail.startswith(".compare<") or tail.startswith(".setFromMask(") or is_assignment(line, match.end()):
                events.append((match.start(), field, "write"))
            else:
                events.append((match.start(), field, "read"))
    for match in XER.finditer(line):
        sub = match.group(1)
        field = f"xer.{sub}" if sub else "xer"
        if sub is None:
            # `compare<...>(a, b, ctx.xer)` copies SO only.
            events.append((match.start(), "xer.so", "benign" if line[match.end():].lstrip().startswith(")") else "read"))
        elif is_assignment(line, match.end()):
            events.append((match.start(), field, "write"))
        else:
            events.append((match.start(), field, "read"))
    for match in RESERVED.finditer(line):
        events.append((match.start(), "reserved", "write" if is_assignment(line, match.end()) else "read"))
    # An assignment's right-hand side is evaluated before its target is
    # written: `ctx.cr6.eq = ctx.cr6.lt` reads first. Put reads that follow
    # the `=` of an assignment ahead of that assignment's write.
    events.sort(key=lambda e: e[0])
    ordered = []
    pending = []
    for position, field, kind in events:
        if kind == "write" and "=" in line[position:]:
            pending.append((field, kind))
        else:
            ordered.append((field, kind))
    return ordered + pending


def scan(files):
    functions = 0
    flagged = {}
    for path in files:
        name = None
        seen = {}
        offset = 0
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            started = FUNCTION.match(line)
            if started:
                name, seen, offset = started.group(1), {}, 0
                functions += 1
                continue
            if name is None:
                continue
            offset += 1
            if line.startswith("}") and not line.startswith("}}"):
                name = None
                continue
            for field, kind in classify_line(line):
                if kind == "benign" or field in seen:
                    continue
                seen[field] = kind
                if kind == "read":
                    flagged.setdefault(field.split(".")[0] if field.startswith("cr") else field, []).append((name, field, offset))
    return functions, flagged


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("directory", type=pathlib.Path)
    parser.add_argument("--limit", type=int, default=40)
    args = parser.parse_args(argv)
    files = sorted(args.directory.glob("ppc_recomp.*.cpp"))
    if not files:
        print(f"promotion check: no generated sources in {args.directory}")
        return 1
    functions, flagged = scan(files)
    print(f"promotion check: {functions} functions in {len(files)} files")
    groups = {"CR": [k for k in flagged if k.startswith("cr")], "XER": [k for k in flagged if k.startswith("xer")],
              "reserved": [k for k in flagged if k == "reserved"]}
    for label, keys in groups.items():
        entries = sorted({(n, f, o) for k in keys for (n, f, o) in flagged[k]})
        names = sorted({n for n, _, _ in entries})
        print(f"promotion check: {label}: {len(names)} functions read a field before writing it")
        for n, f, o in entries[: args.limit]:
            print(f"  {n} {f} first read at line +{o}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
