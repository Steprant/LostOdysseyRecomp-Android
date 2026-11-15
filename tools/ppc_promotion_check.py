#!/usr/bin/env python3
"""Steprant 2.1.1: can CR, XER and the lwarx reservation become locals?

XenonRecomp's cr_as_local, xer_as_local and reserved_as_local give every
generated function its own zero-initialised copy instead of the shared ctx
field. That is exact when every read of a field in a function sees a value
the same function wrote. It changes behaviour where a read can see a value
from outside the function:

- from the caller, on a path from the function entry with no write first;
- from a callee, on a path from a call with no write in between. Compiled
  code preserves cr2-cr4 across calls (non-volatile) and never relies on
  the volatile fields after a call, so a volatile read reached from a call
  means a callee returns something in CR (hand-written code).

This builds the control-flow graph of every generated function (labels,
gotos, conditional gotos, switches, returns) from the sources generated with
the options off and runs a data-flow pass per CR bit, XER carry and the
reservation. It prints function names, the field and the kind of source
(entry or call) only, no code.

Benign reads, not reported:
- mfcr in a prologue saving the CR fields: with locals it saves and later
  restores the function's own copy, and the caller's copy is untouched;
- mfocrf, emitted as a read of cr6 whatever field it names, to save a field
  that is never restored (XenonRecomp has no mtcrf);
- XER summary overflow, copied by every compare and by stwcx./stdcx.: it is
  0 everywhere unless an mtxer sets it, the same with a local.
"""
import argparse
import pathlib
import re
import sys
from collections import defaultdict

FUNCTION = re.compile(r"^PPC_FUNC_IMPL\(__imp__(\w+)\) \{")
LABEL = re.compile(r"^(loc_[0-9A-Fa-f]+):")
GOTO = re.compile(r"goto (loc_[0-9A-Fa-f]+);")
CR = re.compile(r"ctx\.cr([0-7])(?:\.(lt|gt|eq|so))?")
MFCR = re.compile(r"\.u64 \|?= ctx\.cr[0-7]\.(?:lt|gt|eq|so) \? 0x")
# mfocrf, which XenonRecomp emits as a read of cr6 whatever field the
# instruction names (recompiler.cpp: "TODO: don't hardcode to cr6"). Compiled
# code uses it to save a non-volatile field in a prologue; XenonRecomp has no
# mtcrf, so the saved value is never written back to CR. Benign.
MFOCRF = re.compile(r"\.u64 = \(ctx\.cr6\.lt << 7\) \| \(ctx\.cr6\.gt << 6\) \| \(ctx\.cr6\.eq << 5\) \| \(ctx\.cr6\.so << 4\);")
XER_CA = re.compile(r"ctx\.xer\.ca")
RESERVED = re.compile(r"ctx\.reserved(?:\.\w+)?")
CALLEE = re.compile(r"sub_[0-9A-Fa-f]{8}")
CALL = re.compile(r"(?:\bsub_[0-9A-Fa-f]{8}|__imp__\w+|PPC_CALL_INDIRECT_FUNC)\s*\(")
BITS = ("lt", "gt", "eq", "so")
VOLATILE_FIELDS = {0, 1, 5, 6, 7}


def is_assignment(line, end):
    rest = line[end:].lstrip()
    return rest.startswith("=") and not rest.startswith("==")


def statement_events(line):
    """Return ('read'|'write', key) events of one line, reads of an assignment's
    right-hand side before its write. Keys: ('cr', field, bit), ('ca',), ('res',)."""
    reads, writes = [], []
    if not MFCR.search(line) and not MFOCRF.search(line):
        for match in CR.finditer(line):
            field, bit = int(match.group(1)), match.group(2)
            tail = line[match.end():]
            keys = [("cr", field, b) for b in BITS] if bit is None else [("cr", field, bit)]
            # .compare<T>(a, b, xer) for integer compares, .compare(a, b) for
            # fcmpu/fcmpo: both write the whole field.
            if bit is None and (tail.startswith(".compare<") or tail.startswith(".compare(") or tail.startswith(".setFromMask(")):
                writes.extend(keys)
            elif is_assignment(line, match.end()):
                writes.extend(keys)
            else:
                reads.extend(keys)
    for match in XER_CA.finditer(line):
        (writes if is_assignment(line, match.end()) else reads).append(("ca",))
    for match in RESERVED.finditer(line):
        (writes if is_assignment(line, match.end()) else reads).append(("res",))
    return [("read", k) for k in reads] + [("write", k) for k in writes]


def parse_blocks(lines):
    """Split a function body into blocks; return [(statements, successors)].

    A block ends at a label, after a goto, a return, an if with a goto or
    return, and after a whole switch (whose case gotos are its successors)."""
    blocks = [{"body": [], "targets": [], "falls": True}]
    labels = {}
    stripped = [raw.strip() for raw in lines]
    i = 0
    while i < len(stripped):
        line = stripped[i]
        i += 1
        if not line:
            continue
        label = LABEL.match(line)
        if label:
            if blocks[-1]["body"] or blocks[-1]["targets"]:
                blocks.append({"body": [], "targets": [], "falls": True})
            labels[label.group(1)] = len(blocks) - 1
            continue
        current = blocks[-1]
        current["body"].append(line)
        if line.startswith("switch") and line.endswith("{"):
            depth = 1
            unreachable_default = False
            while i < len(stripped) and depth:
                inner = stripped[i]
                i += 1
                depth += inner.count("{") - inner.count("}")
                current["targets"].extend(GOTO.findall(inner))
                if inner.startswith("__builtin_unreachable"):
                    unreachable_default = True
            current["falls"] = not unreachable_default
            blocks.append({"body": [], "targets": [], "falls": True})
            continue
        current["targets"].extend(GOTO.findall(line))
        if line.startswith("goto ") or line.startswith("return") or line.startswith("__builtin_unreachable"):
            current["falls"] = False
            blocks.append({"body": [], "targets": [], "falls": True})
        elif line.startswith("if (") and ("goto " in line or "return" in line):
            blocks.append({"body": [], "targets": [], "falls": True})
    graph = []
    for index, block in enumerate(blocks):
        successors = {labels[t] for t in block["targets"] if t in labels}
        if block["falls"] and index + 1 < len(blocks):
            successors.add(index + 1)
        graph.append((block["body"], successors))
    return graph


KEYS = [("cr", f, b) for f in range(8) for b in BITS] + [("ca",), ("res",)]
SHAPES = {}


def Shape(line):
    """The construct of a flagged line with registers, numbers and names
    masked, so the CI log says which instruction form reads (no game code)."""
    line = re.sub(r"\b(sub|loc)_[0-9A-Fa-f]+", r"\1_X", line)
    line = re.sub(r"\bctx\.(r|f|v)[0-9]+", r"ctx.\1N", line)
    line = re.sub(r"0x[0-9A-Fa-f]+|\b[0-9]+\b", "K", line)
    return line[:120]


def analyse(lines):
    """Return {key: sources} for reads that can see an outside value."""
    graph = parse_blocks(lines)
    state_in = [dict() for _ in graph]
    state_in[0] = {k: frozenset({"entry"}) for k in KEYS}
    work, queued, reached = [0], {0}, {0}
    found = {}
    while work:
        index = work.pop()
        queued.discard(index)
        body, successors = graph[index]
        state = dict(state_in[index])
        for line in body:
            for kind, key in statement_events(line):
                if kind == "read":
                    if state.get(key):
                        found.setdefault(key, set()).update(state[key])
                        SHAPES.setdefault(Shape(line), 0)
                        SHAPES[Shape(line)] += 1
                else:
                    state.pop(key, None)
            if CALL.search(line):
                callee = CALLEE.search(line)
                source = "call:" + (callee.group(0) if callee else "indirect")
                for key in KEYS:
                    if (key[0] == "cr" and key[1] in VOLATILE_FIELDS) or key[0] in ("ca", "res"):
                        state[key] = state.get(key, frozenset()) | {source}
        for successor in successors:
            target = state_in[successor]
            changed = successor not in reached
            reached.add(successor)
            for key, sources in state.items():
                union = target.get(key, frozenset()) | sources
                if union != target.get(key):
                    target[key] = union
                    changed = True
            if changed and successor not in queued:
                work.append(successor)
                queued.add(successor)
    return found


def scan(files):
    functions = 0
    flagged = defaultdict(list)  # key label -> [(name, sources)]
    for path in files:
        name, body = None, []
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            started = FUNCTION.match(line)
            if started:
                name, body = started.group(1), []
                functions += 1
                continue
            if name is None:
                continue
            if line.startswith("}"):
                for key, sources in analyse(body).items():
                    label = f"cr{key[1]}.{key[2]}" if key[0] == "cr" else ("xer.ca" if key[0] == "ca" else "reserved")
                    flagged[label].append((name, "+".join(sorted(sources))))
                name = None
                continue
            body.append(line)
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
    for group, prefix in (("CR", "cr"), ("XER carry", "xer"), ("reservation", "reserved")):
        names = defaultdict(set)
        for label, entries in flagged.items():
            if label.startswith(prefix):
                for name, sources in entries:
                    kinds = "+".join(sorted({s.split(":")[0] for s in sources.split("+")}))
                    names[kinds].add(name)
        total = len(set().union(*names.values())) if names else 0
        detail = ", ".join(f"{len(v)} from {k}" for k, v in sorted(names.items()))
        print(f"promotion check: {group}: {total} functions can read an outside value" + (f" ({detail})" if detail else ""))
    per_field = sorted(((label, len({n for n, _ in entries})) for label, entries in flagged.items()), key=lambda x: -x[1])
    print("promotion check: per field: " + ", ".join(f"{label}={count}" for label, count in per_field[:24]))
    for shape, count in sorted(SHAPES.items(), key=lambda x: -x[1])[:12]:
        print(f"promotion check: shape {count}x: {shape}")
    shown = 0
    for label, entries in sorted(flagged.items()):
        for name, sources in sorted(set(entries)):
            if shown >= args.limit:
                break
            print(f"  {name} {label} from {sources}")
            shown += 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
