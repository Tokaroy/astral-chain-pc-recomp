"""Check that no exclusive window of the game's modules could tell compare-and-swap from the real exclusive monitor.

runtime/cpu.h emulates the exclusive monitor with a compare-and-swap: a store exclusive succeeds when the memory still
holds the value its load exclusive read. Hardware is stricter: any write to the address in between makes the store
fail, also one that puts the same value back (X -> Y -> X, the case tests/excl_mt.py prints). The two differ for the
guest only when the code between the load and the store reads memory or calls something, as a hand-written lock-free
pop does (it reads head->next inside the window). When a window computes from the loaded value and registers alone,
which is what a compiler emits for std::atomic exchange, fetch_add, compare_exchange and lock words, a successful
compare-and-swap gives exactly what hardware gives when the thread runs its window right after the other writers:
nothing read before the load exclusive is protected on hardware either.

This test walks every path from each load exclusive (LDXR, LDAXR, LDXP, LDAXP and their byte and halfword forms) to
the store exclusives it reaches, through direct branches, and fails when:

- a window holds an instruction with a memory operand, a BL, BLR or SVC;
- a load exclusive reaches no store exclusive, or a store exclusive is reached by no load exclusive;
- a path is longer than --limit instructions (the window is then not understood).

Astral Chain 1.0.1 measured on 2026-11-20: main 2290 windows, sdk 841, all clean, 7 instructions at most. A failure
here means the game or SDK code has a window the compare-and-swap may run differently from hardware: the monitor in
cpu.h then needs to see the other threads' exclusive stores (as dynarmic's ExclusiveMonitor does).

Needs the extracted ExeFS. Usage: excl_windows.py [--exefs DIR] [--modules main,sdk] [--limit N] [--samples N]
"""
import argparse
import sys
from collections import Counter
from pathlib import Path

import capstone
from capstone import arm64 as C

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'recomp'))
import layout  # noqa: E402
from module import Module  # noqa: E402

LOADS = {'ldxr', 'ldaxr', 'ldxp', 'ldaxp', 'ldxrb', 'ldxrh', 'ldaxrb', 'ldaxrh'}
STORES = {'stxr', 'stlxr', 'stxp', 'stlxp', 'stxrb', 'stxrh', 'stlxrb', 'stlxrh'}
CALLS = {'bl', 'blr', 'svc'}
ENDS = {'ret', 'br', 'brk', 'udf', 'clrex'}   # the path leaves the function or drops the reservation
PATHS = 20000                                 # paths walked from one load exclusive before giving up


class Windows:
    def __init__(self, mod):
        self.mod = mod
        self.md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        self.md.detail = True
        self.cache = {}

    def insn(self, a):
        if a not in self.cache:
            mod, found = self.mod, None
            if mod.lo <= a < mod.lo + 4 * len(mod.words):
                found = next(self.md.disasm(int(mod.words[(a - mod.lo) // 4]).to_bytes(4, 'little'), a), None)
            self.cache[a] = found
        return self.cache[a]

    def exclusives(self):
        """(load exclusive addresses, store exclusive addresses) of the module's text."""
        mod = self.mod
        sites = [mod.lo + 4 * i for i, w in enumerate(mod.words.tolist()) if (w & 0x3F000000) == 0x08000000]
        named = [(a, self.insn(a)) for a in sites]
        return ([a for a, i in named if i and i.mnemonic in LOADS],
                {a for a, i in named if i and i.mnemonic in STORES})

    def window(self, load, limit):
        """(instructions on a path from load to a store exclusive, the stores reached, problems)."""
        inside, stores, problems = set(), set(), set()
        stack, walked = [(load + 4, (load,))], 0
        while stack:
            pc, path = stack.pop()
            walked += 1
            if walked > PATHS:
                problems.add('too many paths')
                break
            if len(path) > limit:
                problems.add(f'a path longer than {limit} instructions')
                continue
            ins = self.insn(pc)
            if ins is None:
                problems.add('a path leaves the text')
                continue
            name = ins.mnemonic
            if name in STORES:
                stores.add(pc)
                inside.update(path[1:])
                continue
            if name in LOADS or name in ENDS or pc in path:   # another window, the end of this one, or a loop
                continue
            if name == 'b':
                stack.append((ins.operands[0].imm, path + (pc,)))
                continue
            stack.append((pc + 4, path + (pc,)))
            if name.startswith('b.') or name in ('cbz', 'cbnz', 'tbz', 'tbnz'):
                stack.append((ins.operands[-1].imm, path + (pc,)))
        for pc in sorted(inside):
            ins = self.insn(pc)
            if ins.mnemonic in CALLS:
                problems.add(f'{ins.mnemonic} at {pc - self.mod.base:x}')
            elif any(op.type == C.ARM64_OP_MEM for op in ins.operands):
                problems.add(f'memory access at {pc - self.mod.base:x}')
        if not stores:
            problems.add('no store exclusive reached')
        return inside, stores, problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--exefs', type=Path, default=ROOT / 'build' / 'extracted' / 'v101' / 'exefs')
    ap.add_argument('--modules', help='comma-separated (default: every lifted module of the ExeFS)')
    ap.add_argument('--limit', type=int, default=64, help='instructions a window may hold before it counts as unknown')
    ap.add_argument('--samples', type=int, default=8, help='failing windows printed per module')
    args = ap.parse_args()
    bases = {name: base for name, base, _ in layout.layout(args.exefs)}
    failed = 0
    for name in args.modules.split(',') if args.modules else bases:
        mod = Module(name, args.exefs / name, bases[name])
        scan = Windows(mod)
        loads, stores = scan.exclusives()
        reached, sizes, bad = set(), Counter(), []
        for load in loads:
            inside, found, problems = scan.window(load, args.limit)
            reached |= found
            sizes[len(inside)] += 1
            if problems:
                bad.append((load, sorted(inside), sorted(found), sorted(problems)))
        orphans = sorted(stores - reached)
        print(f'== {name}: {len(loads)} load exclusives, {len(stores)} store exclusives, {len(bad)} windows that '
              f'compare-and-swap may run differently, {len(orphans)} stores reached by no load')
        print('   instructions between load and store: ' + ', '.join(f'{n}: {c}' for n, c in sorted(sizes.items())))
        for load, inside, found, problems in bad[:args.samples]:
            print(f'-- window at {load - mod.base:x}: {"; ".join(problems)}')
            for pc in [load] + inside + found:
                ins = scan.insn(pc)
                print(f'       {pc - mod.base:x}: {ins.mnemonic} {ins.op_str}')
        for a in orphans[:args.samples]:
            print(f'-- store exclusive at {a - mod.base:x} is reached by no load exclusive')
        failed += len(bad) + len(orphans)
    print('PASSED' if not failed else f'FAILED: {failed} windows')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
