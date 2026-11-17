#!/usr/bin/env python3
"""Steprant 2.2: can the general registers become function locals?

XenonRecomp's non_argument_as_local (r0, r2, r11, r12, f0, VMX128 v32-v63)
and non_volatile_as_local (r14-r31, f14-f31, v14-v31, v64-v127) give every
generated function its own copy of those registers instead of the shared
PPCContext field. That is exact where every read of such a register sees a
value the same function wrote. It is not where a read can see a value from
outside the function:

- from the caller: a read reached from the function entry with no write first
  (the function relies on a register its caller set, against the ABI);
- from a callee: a read of a volatile register reached from a call with no
  write in between (the callee returns something outside r3/f1/v2).

Benign and not reported: storing a register to the stack (a prologue saving
a non-volatile register: with locals it saves the function's own copy and
the caller's copy is untouched), a register moved to itself (or rN,rN,rN is
a thread priority hint), and the register save/restore helpers (the promoted
build drops their calls; functions keeping their registers in ctx keep them).

The patched XenonRecomp keeps every register in ctx in the functions listed
in registers_in_context_functions. This tool computes that list (analyse, on
sources generated with the two options off) and checks a promoted build
(check):

- every flagged function keeps its registers in ctx;
- the callers that pass it a register do too, and their callers while the
  value at the call site is still the caller's own entry value;
- the callees that return a volatile register do too, and their callees while
  the value comes from a deeper call; the same for a callee-saved register a
  callee writes in ctx without saving it (the movie decoder's VMX swaps);
- for the registers a function reached only through pointers reads from its
  caller (pointer_registers): the function that really wrote the value at
  each indirect call site (the site's own function or a callee); a function
  with locals never touches the ctx copy of the same register, which the
  check verifies;
- every function with setjmp keeps its registers in ctx (ctx = env restores
  ctx, not locals).

A function reached only indirectly cannot be followed to its callers: those
are counted apart, and the registers outside pointer_registers are printed. Only function names, register names and counts are
printed; the generated code is derived from the game and stays out of logs.
"""
import argparse
import json
import tomllib
import pathlib
import re
import sys
from collections import defaultdict

FUNCTION = re.compile(r"^PPC_FUNC_IMPL\(__imp__(\w+)\) \{")
LABEL = re.compile(r"^(loc_[0-9A-Fa-f]+):")
GOTO = re.compile(r"goto (loc_[0-9A-Fa-f]+);")
DIRECT_CALL = re.compile(r"^(sub_[0-9A-Fa-f]{8}|__\w+)\(ctx, base\);")
INDIRECT_CALL = re.compile(r"PPC_CALL_INDIRECT_FUNC\(")
# A register mention: a field access (ctx.r3.u32), a whole-register
# assignment (ctx.v10 = vTemp; from vupkd3d128) or a register passed by
# reference to a mid-asm hook (Hook(ctx.r1, r31)).
REG = re.compile(r"(?<![\w.])(ctx\.)?([rfv])([0-9]+)(?=\.|,|\)| = )")
HOOK_ARGS = re.compile(r"^\w+\((?:(?:ctx\.)?[rfv][0-9]+(?:, )?)+\);$")
SETJMP_CALL = re.compile(r"= setjmp\(")
LONGJMP = re.compile(r"^longjmp\(")
# A store whose address is relative to the stack pointer: a prologue save.
STACK_STORE = re.compile(r"^(?:PPC_STORE_U(?:32|64)\((?:ctx\.)?r1\.u32 \+ -?[0-9]+, |simde_mm_store_si128\(\(simde__m128i\*\)\(base \+ \(\([^)]*\br1\.u32)")
SELF_MOVE = re.compile(r"^(ctx\.)?([rf])([0-9]+)\.u64 = (ctx\.)?([rf])([0-9]+)\.u64;$")
ASSIGN = re.compile(r"^(ctx\.)?([rfv])([0-9]+)(?:\.[a-z0-9]+(?:\[[^\]]*\])?)? (=|\|=|&=|\^=|\+=|-=) ")
VSTORE = re.compile(r"^simde_mm_store\w*\((?:\(simde__m128i?\*\))?(ctx\.)?v([0-9]+)\.")

OFFSET = {"r": 0, "f": 32, "v": 64}


def index(kind, n):
    return OFFSET[kind] + n


def name_of(i):
    if i == CR0:
        return "cr0"
    if i == CTR:
        return "ctr"
    return f"r{i}" if i < 32 else f"f{i - 32}" if i < 64 else f"v{i - 64}"


def promotable(kind, n):
    """The registers the two options turn into locals."""
    if kind == "r":
        return n in (0, 2, 11, 12) or n >= 14
    if kind == "f":
        return n == 0 or n >= 14
    return 32 <= n <= 63 or 14 <= n <= 31 or n >= 64


def volatile(kind, n):
    """Registers a callee may change (r3, f1, v2 carry return values)."""
    if kind == "r":
        return n in (0, 2) or 4 <= n <= 12
    if kind == "f":
        return n == 0 or 2 <= n <= 13
    return n in (0, 1) or 3 <= n <= 13 or 32 <= n <= 63


def mask_of(predicate):
    m = 0
    for kind, count in (("r", 32), ("f", 32), ("v", 128)):
        for n in range(count):
            if predicate(kind, n):
                m |= 1 << index(kind, n)
    return m


CR0, CTR = 192, 193
PROMOTABLE = mask_of(promotable)
VOLATILE = mask_of(volatile) | (1 << CR0) | (1 << CTR)
ALL = (1 << 194) - 1
VOLATILE_BITS = [i for i in range(194) if VOLATILE >> i & 1]
SPECIAL = re.compile(r"(?<![\w.])ctx\.(cr0|ctr)\b")
SPECIAL_WRITE = re.compile(r"^ctx\.(?:(cr0)\.(?:compare|setFromMask)\b|(cr0)\.(?:lt|gt|eq|so) = |(ctr)\.u(?:32|64) = )")


def is_helper(name):
    return name.startswith("__save") or name.startswith("__rest")


def parse_blocks(lines):
    """Blocks of a function body: [(statements, successors)]. A block ends at
    a label, after a goto, a return, a conditional goto or return, a switch,
    and around a conditional tail call (`if (...) {` ... `return;` `}`)."""
    blocks = [{"body": [], "targets": [], "falls": True, "extra": []}]
    labels = {}
    stripped = [raw.split("//", 1)[0].strip() for raw in lines]

    def new_block():
        blocks.append({"body": [], "targets": [], "falls": True, "extra": []})

    i = 0
    while i < len(stripped):
        line = stripped[i]
        i += 1
        if not line:
            continue
        label = LABEL.match(line)
        if label:
            if blocks[-1]["body"] or blocks[-1]["targets"]:
                new_block()
            labels[label.group(1)] = len(blocks) - 1
            continue
        current = blocks[-1]
        if line.startswith("if (") and line.endswith("{"):
            current["body"].append(line)
            before = len(blocks) - 1
            new_block()
            inner = len(blocks) - 1
            depth = 1
            while i < len(stripped) and depth:
                inner_line = stripped[i]
                i += 1
                depth += inner_line.count("{") - inner_line.count("}")
                if depth and inner_line:
                    blocks[inner]["body"].append(inner_line)
            blocks[inner]["falls"] = False
            new_block()
            blocks[before]["falls"] = False
            blocks[before]["extra"] = [inner, len(blocks) - 1]
            continue
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
            new_block()
            continue
        current["targets"].extend(GOTO.findall(line))
        if line.startswith("goto ") or line.startswith("return") or line.startswith("__builtin_unreachable") or LONGJMP.match(line):
            current["falls"] = False
            new_block()
        elif line.startswith("if (") and ("goto " in line or "return" in line):
            new_block()
    graph = []
    for number, block in enumerate(blocks):
        successors = {labels[t] for t in block["targets"] if t in labels}
        successors.update(block["extra"])
        if block["falls"] and number + 1 < len(blocks):
            successors.add(number + 1)
        graph.append((block["body"], successors))
    return graph


def events(line):
    """(reads, write, forms) of a statement as register indices; forms lists
    (kind, n, in_ctx) of every mention. Element writes (one lane) count as
    writes: compiled code does not keep lanes of a caller's vector register."""
    events.hook_writes = []
    events.write_in_ctx = False
    if STACK_STORE.match(line):
        return (), None, ()
    move = SELF_MOVE.match(line)
    if move and move.group(1) == move.group(4) and move.group(2, 3) == move.group(5, 6):
        return (), None, ()
    written = None
    skip_at = -1
    m = ASSIGN.match(line)
    if m:
        written = (m.group(2), int(m.group(3)), bool(m.group(1)))
        skip_at = 0 if m.group(4) == "=" else -1
    else:
        vm = VSTORE.match(line)
        if vm:
            written = ("v", int(vm.group(2)), bool(vm.group(1)))
            skip_at = vm.start(1) if vm.group(1) else vm.start(2) - 1
    reads = []
    forms = []
    for r in REG.finditer(line):
        key = (r.group(2), int(r.group(3)), bool(r.group(1)))
        forms.append(key)
        if written and key == written and r.start() == skip_at:
            continue
        reads.append(index(key[0], key[1]))
    write = index(written[0], written[1]) if written else None
    events.write_in_ctx = bool(written and written[2])
    if HOOK_ARGS.match(line) and not DIRECT_CALL.match(line):
        events.hook_writes = [index(k, n) for k, n, _ in forms]
    else:
        events.hook_writes = []
    special = SPECIAL_WRITE.match(line)
    special_write = None
    if special:
        special_write = CR0 if (special.group(1) or special.group(2)) else CTR
    for match in SPECIAL.finditer(line):
        bit = CR0 if match.group(1) == "cr0" else CTR
        if special_write == bit and match.start() == 0:
            continue
        reads.append(bit)
    if special_write is not None:
        if write is None:
            write = special_write
        else:
            reads.append(special_write)  # rare: two writes; keep it conservative
    return reads, write, forms


class Function:
    __slots__ = ("name", "entry_reads", "call_reads", "site_entry", "site_calls",
                 "ret_entry", "ret_calls", "local", "ctx", "setjmp", "callees", "indirect_reads", "ctx_needs",
                 "indirect_entry", "indirect_written", "indirect_calls", "site_written", "unsaved")

    def __init__(self, name):
        self.name = name
        self.entry_reads = 0                 # mask: reads that can see the caller's value
        self.call_reads = defaultdict(set)   # register -> callees whose value a read can see
        self.indirect_reads = 0              # mask: reads that can see an indirect callee's value
        self.site_entry = defaultdict(int)   # callee -> registers still holding our entry value at the call
        self.site_calls = {}                 # callee -> {register: callees} at the call
        self.ret_entry = 0                   # mask at the returns: our entry value
        self.ret_calls = defaultdict(set)    # register -> callees whose value we return
        self.local = 0                       # registers used as locals
        self.ctx = 0                         # registers used in ctx
        self.setjmp = False
        self.callees = set()
        self.ctx_needs = 0                   # registers a host patch reads from this function
        self.indirect_entry = 0              # at indirect calls: registers that may hold our entry value
        self.indirect_written = 0            # at indirect calls: registers we may have written
        self.indirect_calls = {}             # at indirect calls: {register: callees whose value it may hold}
        self.site_written = defaultdict(int) # callee -> registers we may have written, at the call
        self.unsaved = 0                     # callee-saved registers written without a save first


HIGH_VMX = sum(1 << index("v", n) for n in range(64, 128))
NONVOLATILE = mask_of(lambda kind, n: (kind in "rf" and n >= 14) or (kind == "v" and (14 <= n <= 31 or n >= 64)))
SAVE_HELPER = re.compile(r"^__save(gprlr|fpr|vmx)_([0-9]+)\(ctx, base\);")
VECTOR_FRAME_STORE = re.compile(r"^simde_mm_store_si128\(\(simde__m128i\*\)\(base \+ \(\((?:ctx\.)?r(?:1|11|12)\.u32")


def first_pass(lines):
    """Registers a function can leave changed for its caller (volatile ones it
    writes, callee-saved ones it writes without saving them first), its
    direct callees, whether it makes indirect calls, and the callee-saved
    registers it saves."""
    writes, shared, saved, callees, indirect = 0, 0, 0, set(), False
    # Only blocks reachable from the entry: a jump table XenonRecomp did not
    # recognise is decoded as code after the bctr (0x8229xxxx words read as
    # lwz r17,...), and those writes never happen.
    graph = parse_blocks(lines)
    seen, stack = {0}, [0]
    while stack:
        for successor in graph[stack.pop()][1]:
            if successor not in seen:
                seen.add(successor)
                stack.append(successor)
    for line in (line for number in sorted(seen) for line in graph[number][0]):
        if not line:
            continue
        helper = SAVE_HELPER.match(line)
        if helper:
            kind = {"gprlr": "r", "fpr": "f", "vmx": "v"}[helper.group(1)]
            first = int(helper.group(2))
            for n in range(first, 128 if kind == "v" and first >= 64 else 32):
                saved |= 1 << index(kind, n)
        if STACK_STORE.match(line) or VECTOR_FRAME_STORE.match(line):
            for r in REG.finditer(line):
                saved |= 1 << index(r.group(2), int(r.group(3)))
            continue
        _, write, _ = events(line)
        if write is not None:
            writes |= 1 << write
            if events.write_in_ctx:
                shared |= 1 << write
        for r in events.hook_writes:
            writes |= 1 << r
        callee = DIRECT_CALL.match(line)
        if callee:
            if not is_helper(callee.group(1)):
                callees.add(callee.group(1))
        elif INDIRECT_CALL.search(line):
            indirect = True
    # A callee-saved register written as a local (a promoted build, where the
    # save helpers are dropped too) never reaches the caller.
    return (writes & VOLATILE) | (shared & NONVOLATILE & ~saved), saved, callees, indirect


def may_write(first):
    """Registers each function can leave changed for its caller: volatile
    registers it or anything it calls writes (all of them through an
    indirect call or an unknown callee), and callee-saved registers written
    without a save (sub_83111958 and sub_83111CA0 load v64-v127 for the
    movie decoder), to a fixed point."""
    result = {name: writes | (VOLATILE if indirect else 0) for name, (writes, _, _, indirect) in first.items()}
    callers = defaultdict(set)
    for name, (_, _, callees, _) in first.items():
        for callee in callees:
            callers[callee].add(name)
    work = list(first)
    while work:
        name = work.pop()
        mask = result[name]
        saved = first[name][1]
        for callee in first[name][2]:
            mask |= result.get(callee, VOLATILE) & ~saved
        if mask != result[name]:
            result[name] = mask
            work.extend(callers.get(name, ()))
    return result


MAY_WRITE = {}
FIRST = {}


def analyse_function(name, lines):
    fn = Function(name)
    graph = parse_blocks(lines)
    # State: (mask of registers that may hold the entry value,
    #         {register: frozenset(callees)} for values from a callee).
    states = {0: (ALL, {}, 0)}
    work, queued = [0], {0}
    while work:
        number = work.pop()
        queued.discard(number)
        body, successors = graph[number]
        entry_mask, calls, written = states[number]
        calls = dict(calls)
        for line in body:
            if SETJMP_CALL.search(line):
                fn.setjmp = True
            reads, write, forms = events(line)
            for kind, n, in_ctx in forms:
                bit = 1 << index(kind, n)
                if in_ctx:
                    fn.ctx |= bit
                else:
                    fn.local |= bit
            for r in reads:
                if entry_mask >> r & 1:
                    fn.entry_reads |= 1 << r
                if r in calls:
                    fn.call_reads[r] |= calls[r]
            if write is not None:
                entry_mask &= ~(1 << write)
                calls.pop(write, None)
                written |= 1 << write
            for r in events.hook_writes:
                written |= 1 << r
            callee = DIRECT_CALL.match(line)
            if callee or INDIRECT_CALL.search(line):
                target = callee.group(1) if callee else "indirect"
                if not is_helper(target):
                    if callee:
                        fn.callees.add(target)
                        fn.site_entry[target] |= entry_mask
                        fn.site_written[target] |= written
                        site = fn.site_calls.setdefault(target, {})
                        for r, owners in calls.items():
                            site.setdefault(r, set()).update(owners)
                    else:
                        fn.indirect_entry |= entry_mask
                        fn.indirect_written |= written
                        for r, owners in calls.items():
                            fn.indirect_calls.setdefault(r, set()).update(owners)
                    changed_by_call = MAY_WRITE.get(target, VOLATILE) if callee else VOLATILE
                    # A volatile register the callee may write is the
                    # callee's afterwards. A callee-saved one it writes
                    # without a save may be its value or still the old one
                    # (a save the scan does not see): both.
                    entry_mask &= ~(changed_by_call & VOLATILE)
                    tag = frozenset({target})
                    bits = changed_by_call
                    while bits:
                        low = bits & -bits
                        r = low.bit_length() - 1
                        if low & VOLATILE:
                            calls[r] = tag
                        else:
                            calls[r] = calls.get(r, frozenset()) | tag
                        bits ^= low
            if line.startswith("return") or (line.startswith("if (") and "return" in line):
                fn.ret_entry |= entry_mask
                for r, owners in calls.items():
                    fn.ret_calls[r] |= owners
        for successor in successors:
            old = states.get(successor)
            if old is None:
                states[successor] = (entry_mask, dict(calls), written)
                changed = True
            else:
                old_mask, old_calls, old_written = old
                new_mask = old_mask | entry_mask
                new_written = old_written | written
                changed = new_mask != old_mask or new_written != old_written
                merged = dict(old_calls)
                for r, owners in calls.items():
                    union = merged.get(r, frozenset()) | owners
                    if union != merged.get(r):
                        merged[r] = union
                        changed = True
                if changed:
                    states[successor] = (new_mask, merged, new_written)
            if changed and successor not in queued:
                work.append(successor)
                queued.add(successor)
    for r in list(fn.call_reads):
        if "indirect" in fn.call_reads[r]:
            fn.indirect_reads |= 1 << r
    return fn


def bodies(directory):
    for path in sorted(pathlib.Path(directory).glob("ppc_recomp.*.cpp")):
        name, body = None, []
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            started = FUNCTION.match(line)
            if started:
                name, body = started.group(1), []
                continue
            if name is None:
                continue
            if line.startswith("}"):
                yield name, body
                name = None
                continue
            body.append(line)


def scan(directory):
    first = {name: first_pass(body) for name, body in bodies(directory)}
    FIRST.clear()
    FIRST.update(first)
    MAY_WRITE.clear()
    MAY_WRITE.update(may_write(first))
    functions = {name: analyse_function(name, body) for name, body in bodies(directory)}
    for name, fn in functions.items():
        fn.unsaved = first[name][0] & NONVOLATILE
    return functions


def closure(functions, preserves, replaced, forced=(), saves=()):
    # closure.pointer_registers: mask of the registers the pointer rule
    # applies to (None: all of them), set by main from the config.
    """{function: reason} of the functions that must keep their registers in
    ctx, with entry_needs and return_needs as register masks."""
    callers = defaultdict(set)
    for fn in functions.values():
        for callee in fn.callees:
            callers[callee].add(fn.name)
    need = {}
    entry_needs = defaultdict(int)
    return_needs = defaultdict(int)
    pass_needs = defaultdict(int)    # registers a caller must hold in ctx at a call
    work = []

    def add(name, why):
        if name in functions and not is_helper(name) and name not in replaced and name not in need:
            need[name] = why

    def wants_return(owner, r):
        if owner != "indirect" and owner not in replaced and r not in preserves.get(owner, ()) \
                and not return_needs[owner] >> r & 1:
            return_needs[owner] |= 1 << r
            work.append(("return", owner))

    for name in forced:
        add(name, "forced")
    for fn in functions.values():
        if is_helper(fn.name) or fn.name in replaced:
            continue
        if fn.setjmp:
            add(fn.name, "setjmp")
        if fn.unsaved & HIGH_VMX:
            # v64-v127 written for the caller: the VMX register-file swaps of
            # the movie decoder (sub_83111958, sub_83111CA0) and the guest
            # longjmp. Their writes must reach ctx whoever reads them.
            add(fn.name, "loads registers for its caller")
        if fn.entry_reads & PROMOTABLE and fn.name not in saves:
            add(fn.name, "reads from its caller")
            entry_needs[fn.name] |= fn.entry_reads & PROMOTABLE
            work.append(("entry", fn.name))
        for r, owners in fn.call_reads.items():
            if not (PROMOTABLE >> r & 1):
                continue
            kept = {o for o in owners if o == "indirect" or (o not in replaced and r not in preserves.get(o, ()))}
            if kept:
                add(fn.name, "reads from a callee")
            for owner in kept:
                wants_return(owner, r)
    # carry_needs: registers a function may carry from its entry to an
    # indirect call site where a function reached only through pointers may
    # read them (the pointer rule below).
    carry_needs = defaultdict(int)

    def drain():
        while work:
            kind, name = work.pop()
            if kind == "entry":
                # A caller passing its own entry value keeps its registers in
                # ctx too, and so on up while the value is still the entry
                # value (the rule of the first 2.2).
                regs = entry_needs[name]
                for caller in callers.get(name, ()):
                    add(caller, "passes registers")
                    pass_needs[caller] |= regs
                    cfn = functions[caller]
                    passed = cfn.site_entry.get(name, 0) & regs
                    if passed & ~entry_needs[caller]:
                        entry_needs[caller] |= passed
                        work.append(("entry", caller))
                    for r, owners in cfn.site_calls.get(name, {}).items():
                        if regs >> r & 1:
                            for owner in owners:
                                wants_return(owner, r)
            elif kind == "carry":
                # Through function pointers: a function keeping its registers
                # as locals never touches their ctx copies (check fails on a
                # function that does), so the value found in ctx at the
                # indirect site is the last one a ctx function wrote. Only the
                # function that really wrote it needs ctx: the caller holding
                # its own write at the call, or the callee whose value it
                # holds there (return_needs). A caller that carries its own
                # entry value passes the need up without ctx.
                regs = carry_needs[name]
                for caller in callers.get(name, ()):
                    cfn = functions[caller]
                    own = cfn.site_written.get(name, 0) & regs
                    if own:
                        add(caller, "sets registers for a function pointer")
                        pass_needs[caller] |= own
                    passed = cfn.site_entry.get(name, 0) & regs
                    if passed & ~carry_needs[caller]:
                        carry_needs[caller] |= passed
                        work.append(("carry", caller))
                    for r, owners in cfn.site_calls.get(name, {}).items():
                        if regs >> r & 1:
                            for owner in owners:
                                wants_return(owner, r)
            else:
                fn = functions.get(name)
                if fn is None or is_helper(name) or name in replaced:
                    continue
                add(name, "returns registers")
                regs = return_needs[name]
                through = fn.ret_entry & regs
                if through & ~entry_needs[name]:
                    entry_needs[name] |= through
                    work.append(("entry", name))
                for r, owners in fn.ret_calls.items():
                    if regs >> r & 1:
                        for owner in owners:
                            wants_return(owner, r)

    drain()
    # A function reached only through pointers that reads registers from its
    # caller: any indirect call site may be that caller (the "carry" rule
    # above). The registers come from closure.pointer_registers (None: all):
    # the entry reads of indirect-only functions outside it are paths that
    # do not happen (see register_check.toml). The EH funclets (read r12
    # only, never called directly) are not called through the game's
    # pointers.
    indirect_regs = 0
    closure.left_out = 0
    while True:
        wanted = 0
        for n, m in entry_needs.items():
            if m and not callers.get(n) and not (m == 1 << index("r", 12)):
                wanted |= m
        if getattr(closure, "pointer_registers", None) is not None:
            closure.left_out |= wanted & ~closure.pointer_registers
            wanted &= closure.pointer_registers
        if wanted == indirect_regs:
            break
        indirect_regs = wanted
        for fn in functions.values():
            if is_helper(fn.name) or fn.name in replaced:
                continue
            own = fn.indirect_written & indirect_regs
            if own:
                add(fn.name, "sets registers for a function pointer")
                pass_needs[fn.name] |= own
            up = fn.indirect_entry & indirect_regs & ~carry_needs[fn.name]
            if up:
                carry_needs[fn.name] |= up
                work.append(("carry", fn.name))
            for r, owners in fn.indirect_calls.items():
                if indirect_regs >> r & 1:
                    for owner in owners:
                        wants_return(owner, r)
        drain()
    closure.indirect_regs = indirect_regs
    closure.carry_needs = carry_needs
    unreachable = sorted(n for n, m in entry_needs.items() if m and not callers.get(n))
    indirect_returns = sorted(n for n, fn in functions.items() if fn.indirect_reads & PROMOTABLE and n not in replaced)
    for name, regs in pass_needs.items():
        entry_needs.setdefault(name, 0)
    closure.pass_needs = pass_needs
    return need, entry_needs, return_needs, unreachable, indirect_returns


def mapping(directory):
    """Function name -> guest address, from ppc_func_mapping.cpp."""
    result = {}
    text = (pathlib.Path(directory) / "ppc_func_mapping.cpp").read_text(encoding="utf-8", errors="replace")
    for match in re.finditer(r"\{ 0x([0-9A-Fa-f]+), (\w+) \}", text):
        result[match.group(2)] = int(match.group(1), 16)
    return result


def listed(path):
    return {int(m, 16) for m in re.findall(r"0x([0-9A-Fa-f]+)", path.read_text())}


def masks_to_names(mask):
    return [name_of(i) for i in range(194) if mask >> i & 1]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("mode", choices=["analyse", "check"])
    parser.add_argument("directory", type=pathlib.Path)
    parser.add_argument("--preserves", action="append", default=[],
                        help="FUNC:REG,REG: a callee that leaves these volatile registers unchanged")
    parser.add_argument("--replaced", action="append", default=[],
                        help="function replaced by a native override that reads no register but the arguments")
    parser.add_argument("--config", type=pathlib.Path,
                        help="TOML with native, replaced, preserves and site_reads lists (LostOdysseyRecompLib/config/register_check.toml)")
    parser.add_argument("--native", action="append", default=[],
                        help="function replaced by a native override that changes r3 and memory only: "
                             "no caller may read another volatile register (cr0 and ctr included) after it")
    parser.add_argument("--entry-reads", action="append", default=[],
                        help="FUNC:REG,REG: a host override of FUNC reads these registers of its caller")
    parser.add_argument("--site-reads", action="append", default=[],
                        help="CALLER:CALLEE:REG,REG: a host override of CALLEE reads these registers when called from CALLER "
                             "(it checks the return address): CALLER keeps them in ctx, and passes on those it got from its own caller")
    parser.add_argument("--pointer-registers", nargs="*", default=None,
                        help="registers the function-pointer rule applies to (default: all)")
    parser.add_argument("--saves", action="append", default=[],
                        help="function that stores the caller's registers to a save area: its entry reads are saves")
    parser.add_argument("--force", action="append", default=[],
                        help="function kept in ctx for a host patch that reads its registers")
    parser.add_argument("--json", type=pathlib.Path, help="analyse: write the details here")
    parser.add_argument("--write-list", type=pathlib.Path,
                        help="analyse: write the registers_in_context file for XenonRecomp here")
    parser.add_argument("--list", type=pathlib.Path,
                        help="check: the registers_in_context file the sources were generated with; "
                             "fails when it misses a function the analysis needs")
    parser.add_argument("--limit", type=int, default=40)
    parser.add_argument("--strict", action="store_true")
    args = parser.parse_args(argv)
    if args.config:
        config = tomllib.loads(args.config.read_text(encoding="utf-8"))
        args.native += config.get("native", [])
        args.replaced += config.get("replaced", [])
        args.preserves += config.get("preserves", [])
        args.site_reads += config.get("site_reads", [])
        args.force += config.get("force", [])
        args.saves += config.get("saves", [])
        if "pointer_registers" in config:
            args.pointer_registers = config["pointer_registers"]
    preserves = defaultdict(set)
    for item in args.preserves:
        name, regs = item.split(":")
        for reg in regs.split(","):
            preserves[name].add(index(reg[0], int(reg[1:])))
    replaced = set(args.replaced) | set(args.native)
    closure.pointer_registers = None
    if args.pointer_registers is not None:
        closure.pointer_registers = 0
        for item in args.pointer_registers:
            first, _, last = item.partition("-")
            last = last or first
            for n in range(int(first[1:]), int(last[1:]) + 1):
                closure.pointer_registers |= 1 << index(first[0], n)
    functions = scan(args.directory)
    for item in args.entry_reads:
        name, regs = item.split(":")
        for reg in regs.split(","):
            functions[name].entry_reads |= 1 << index(reg[0], int(reg[1:]))
    forced = list(args.force)
    for item in args.site_reads:
        caller, callee, regs = item.split(":")
        mask = 0
        for reg in regs.split(","):
            mask |= 1 << index(reg[0], int(reg[1:]))
        if callee not in functions[caller].callees:
            print(f"register check: {caller} does not call {callee}")
            return 1
        forced.append(caller)
        functions[caller].entry_reads |= functions[caller].site_entry.get(callee, 0) & mask
        functions[caller].ctx_needs = mask
    native_reads = []
    for fn in functions.values():
        for r, owners in fn.call_reads.items():
            hit = sorted(o for o in owners if o in args.native)
            if hit and r != index("r", 3):
                native_reads.append((fn.name, name_of(r), hit[0]))
    print(f"register check: native overrides ({len(args.native)}): {len(native_reads)} reads of a register they leave unchanged")
    for name, reg, callee in native_reads[:args.limit]:
        print(f"  {name} reads {reg} after {callee}")
    if not functions:
        print(f"register check: no generated sources in {args.directory}")
        return 1
    print(f"register check: {len(functions)} functions")
    need, entry_needs, return_needs, unreachable, indirect_returns = closure(functions, preserves, replaced, forced, set(args.saves))
    reasons = defaultdict(int)
    for why in need.values():
        reasons[why] += 1
    print(f"register check: {len(need)} functions keep the registers in ctx (" +
          ", ".join(f"{k}: {v}" for k, v in sorted(reasons.items())) + ")")
    print(f"register check: registers passed through function pointers: {', '.join(masks_to_names(closure.indirect_regs)) or 'none'}"
          + (f"; left out by pointer_registers: {', '.join(masks_to_names(closure.left_out))}" if closure.left_out else ""))
    print(f"register check: {len(unreachable)} of them read registers from a caller but are never called directly; "
          f"{len(indirect_returns)} read a volatile register after an indirect call")
    if args.mode == "analyse":
        if args.write_list:
            addresses = mapping(args.directory)
            missing = sorted(n for n in need if n not in addresses)
            if missing:
                print(f"register check: no address for {len(missing)} functions: {', '.join(missing[:5])}")
                return 1
            counts = defaultdict(int)
            for why in need.values():
                counts[why] += 1
            lines = [
                "# Steprant 2.2: functions that keep every general, float and vector register",
                "# in ctx under non_argument_as_local/non_volatile_as_local. Written by",
                "#   python tools/ppc_register_check.py analyse <sources generated with both options off> --write-list ...",
                "# (the options used are in .github/workflows/android-apk.yml). Do not edit by hand.",
                "# " + ", ".join(f"{k}: {v}" for k, v in sorted(counts.items())),
                "[registers_in_context]",
                "functions = [",
            ]
            lines += [f"    0x{addresses[n]:08X}," for n in sorted(need, key=lambda n: addresses[n])]
            lines.append("]")
            args.write_list.write_text("\n".join(lines) + "\n")
            print(f"register check: wrote {len(need)} functions to {args.write_list.name}")
        if args.json:
            args.json.write_text(json.dumps({
                "functions": {n: need[n] for n in sorted(need)},
                "entry_needs": {n: masks_to_names(m) for n, m in sorted(entry_needs.items()) if m},
                "return_needs": {n: masks_to_names(m) for n, m in sorted(return_needs.items()) if m},
                "unreachable": unreachable,
                "indirect_returns": indirect_returns,
            }, indent=1))
        return 2 if (args.strict and native_reads) else 0
    # check: every function the closure names must keep the needed registers
    # in ctx; with setjmp, all of them.
    problems = []
    for name in sorted(need):
        fn = functions[name]
        wanted = (entry_needs.get(name, 0) | return_needs.get(name, 0) | closure.pass_needs.get(name, 0) | fn.entry_reads | fn.ctx_needs
                  | sum(1 << r for r in fn.call_reads)) & PROMOTABLE
        if fn.setjmp:
            wanted |= PROMOTABLE
        bad = fn.local & wanted
        if bad:
            problems.append((name, ",".join(masks_to_names(bad)[:6]), need[name]))
    problems += [(name, reg, "reads it after native " + callee) for name, reg, callee in native_reads]
    # The closure relies on a function with locals never touching the ctx
    # copy of the same register.
    mixed = sorted(n for n, fn in functions.items() if fn.local & fn.ctx & PROMOTABLE)
    print(f"register check: {len(mixed)} functions use a register both as a local and in ctx")
    problems += [(n, ",".join(masks_to_names(functions[n].local & functions[n].ctx & PROMOTABLE)[:6]),
                  "local and ctx copy of the same register") for n in mixed]
    if args.list:
        addresses = mapping(args.directory)
        in_list = listed(args.list)
        missing = sorted(n for n in need if addresses.get(n) not in in_list)
        print(f"register check: list has {len(in_list)} functions; {len(missing)} needed ones missing")
        problems += [(n, "-", "missing from the list") for n in missing]
    print(f"register check: verdict: {'clean' if not problems else f'{len(problems)} open'}")
    for name, regs, why in problems[:args.limit]:
        print(f"  {name} {regs} ({why})")
    return 2 if (args.strict and problems) else 0


if __name__ == "__main__":
    sys.exit(main())
