#!/usr/bin/env python3

from itertools import batched
import re
from typing import DefaultDict, Iterable

MAX_IMM = (1 << 25) - 1

ASMRE = re.compile(r"""
    \s*(?:
        (?P<inv>0x[0-9a-fA-F]+)|
        (?P<op>\w+)\s* (?P<args>[^;]*)|
        "(?P<str>(?:\\x[\da-fA-F].|.)+)"
    )?
    (?:; (?P<comment>.*))?
""", re.VERBOSE)

CHARRE = re.compile(r"^'(?P<char>.)'$")

CANONICAL = [
    "cmov",
    "aidx", "aupd",
    "add", "mul", "div", "nand",
    "halt",
    "alloc", "dealloc",
    "out", "in",
    "loadprog", "loadimm"
]

ARGC: dict[str, int] = { # name: argc
    "mov": 3,
    "lda": 3, "sta": 3,
    "add": 3, "mul": 3, "div": 3, "nan": 3,
    "hlt": 0,
    "new": 2, "del": 1,
    "out": 1, "inp": 1,
    "prg": 2, "ldi": 2
}
NAMES = list[str](ARGC.keys())
CODES = dict[str, int]()
for i, (canon, (name, argc)) in enumerate(zip(CANONICAL, list(ARGC.items()))):
    CODES[canon] = i
    CODES[name] = i
    ARGC[canon] = argc

def parse_value(value: str):
    if m := CHARRE.match(value):
        return ord(m['char'])
    return int(value, 0)

def progasm(lines: Iterable[str]):
    symtab = dict[str, int]() # label: address
    patches = DefaultDict[str, set[int]](set) # label: addrs to patch
    data = list[int]()

    for line in lines:
        if not (line := line.strip()):
            continue
        
        if (m := ASMRE.match(line)) is None:
            raise ValueError(f"Invalid assembly line: {line.strip()}")

        if (op := m['op']) is None:
            if inv := m['inv']:
                code = int(inv, 16)
            elif s := m['str']:
                s = re.sub(
                    r"\\x([\da-fA-F]{2})",
                    lambda m: chr(int(m[1], 16)),
                    s
                )
                s += "\0"*((4 - len(s) % 4)%4)
                for a, b, c, d in batched((ord(c) for c in s), 4):
                    data.append(a<<24 | b<<16 | c<<8 | d)
                continue
            else:
                continue
        else:
            op = op.lower()
            if m['args']:
                args = re.split(r"\s+", m['args'].strip())
            else:
                args = []
            
            if op in {"ldi", "loadimm"}:
                reg, imm = args
                if imm.startswith("@"):
                    imm = imm[1:]
                    if (addr := symtab.get(imm)) is not None:
                        imm = addr
                    else:
                        patches[imm].add(len(data))
                        imm = 0 # Placeholder
                else:
                    imm = parse_value(imm)
                
                code = 13 << 28 | (int(reg) & 7) << 25 | int(imm) & MAX_IMM
            elif op == "label":
                label, = args
                if not label.startswith("@"):
                    raise ValueError(f"Invalid label: {label}")
                
                label = label[1:]  # Remove '@' prefix
                if label in symtab:
                    raise ValueError(f"Duplicate label: {label}")
                symtab[label] = len(data)
                continue
            else:
                argc = ARGC[op]
                if len(args) != argc:
                    raise ValueError(
                        "Invalid number of arguments for "
                        f"{op}: {len(args)} ({argc} expected)"
                    )
                
                try:
                    code = CODES[op] << 28
                except KeyError:
                    raise ValueError(f"Unknown instruction: {op}") from None
                
                for i, arg in enumerate(args):
                    if arg.startswith("@"):
                        label = arg[1:]
                        if label in symtab:
                            arg = symtab[label]
                        else:
                            patches[label].add(len(data))
                            arg = 0
                    else:
                        arg = parse_value(arg)
                    
                    code |= (arg & 7) << (3*(len(args) - i - 1))

            data.append(code)
        
        if len(data) > MAX_IMM:
            raise ValueError("Program size exceeds 25-bit limit")
    
    for label, addrs in patches.items():
        if label not in symtab:
            raise ValueError(f"Undefined label: @{label}")
        addr = symtab[label]
        for a in addrs:
            data[a] |= addr

    return data

def main_asm(inp, out):
    try:
        with open(inp, 'r') as inpf:
            with open(out, "wb") as outf:
                xx = progasm(inpf)
                for op in xx:#progasm(inpf):
                    outf.write(op.to_bytes(4, 'big'))

    except FileNotFoundError:
        print(f"File {inp} not found.")
        return 1
    
    return 0

def is_canonical(code: int):
    # ldi is always canonical because it uses all bits
    if code >> 28 == 13: return True
    # Mask out only the register bits used by the instruction
    mask = (1 << (ARGC[NAMES[code >> 28]]*3)) - 1
    return not (code & MAX_IMM & ~mask)

def opdis(code: int):
    op = code >> 28
    if op == 13:
        return ("ldi", (code >> 25) & 7, code & MAX_IMM)

    if op < 13:
        abc = (code >> 6) & 7, (code >> 3) & 7, code & 7
        name = NAMES[op]
        count = ARGC[name]
        return (name, abc[3 - count:])
    
    return code

def main_dis(fname):
    try:
        with open(fname, 'rb') as file:
            prog = file.read()
    except FileNotFoundError:
        print(f"File {fname} not found.")
        return 1
    
    for a, b, c, d in batched(prog, 4):
        code = a<<24 | b<<16 | c<<8 | d
        match opdis(code):
            case int(code):
                print(f"0x{code:08x}")
            case ("ldi", reg, imm):
                if 0x20 <= imm < 0x7f:
                    print(f"ldi {reg} 0x{imm:02x} ; '{chr(imm)}'")
                else:
                    print(f"ldi {reg} 0x{imm:02x}")
            case (name, regs):
                line = name
                if regs:
                    line += f" {' '.join(map(str, regs))}"
                if is_canonical(code):
                    print(line)
                else:
                    print(f"0x{code:08x} ; {line}")

    return 0

def main(*argv):
    match argv[1:]:
        case ["asm", inp, out]: return main_asm(inp, out)
        case ["dis", inp]: return main_dis(inp)

        case _:
            print(f"Usage: python {argv[0]} <command> ...")
            print("Commands:")
            print("  asm <in> <out>   Assemble code")
            print("  dis <in>         Disassemble binary")
            return 1

if __name__ == "__main__":
    import sys
    main(*sys.argv)