from itertools import batched
import re
from typing import Iterable

ASMRE = re.compile(r"""
    \s*(?:
        (?P<inv>0x[0-9a-fA-F]+)|
        (?P<op>\w+)\s*
        (?P<args>[^;]*)
    )?
    (; (?P<comment>.*))?
""", re.VERBOSE)

CHARRE = re.compile(r"^'(?P<char>.)'$")

OPCODES: dict[str, int] = { # name: registers
    "mov": 3,
    "lda": 3, "sta": 3,
    "add": 3, "mul": 3, "div": 3, "nand": 3,
    "hlt": 0,
    "new": 2, "del": 1,
    "out": 1, "inp": 1,
    "prg": 2, "ldi": 2
}
NAMES = list(OPCODES.keys())
CODES = dict(enumerate(OPCODES.items()))

def codeof(name: str):
    try: return NAMES.index(name.lower())
    except ValueError:
        return "invalid"

def parse_value(value: str):
    if m := CHARRE.match(value):
        return ord(m['char'])
    return int(value, 0)

def opasm(line: str):
    if (m := ASMRE.match(line)) is None:
        raise ValueError(f"Invalid assembly line: {line.strip()}")

    if (op := m['op']) is None:
        if inv := m['inv']:
            return int(inv, 16)
        return None  # Empty line or comment
    
    op = op.lower()
    argc = OPCODES[op]
    if m['args']:
        args = [parse_value(arg) for arg in m['args'].split(',')]
    else:
        args = []
    if len(args) != argc:
        raise ValueError(f"Invalid number of arguments for {op}: {len(args)} ({argc} expected)")
    
    code = NAMES.index(op) << 28
    if op == "ldi":
        reg, imm = args
        code |= (int(reg) & 0x07) << 24
        code |= int(imm) & 0x01FF_FFFF
    else:
        for i, arg in enumerate(args):
            code |= int(arg) << (3*i)
    
    return code

def progasm(lines: Iterable[str]):
    for line in lines:
        if line := line.strip():
            if (op := opasm(line)) is not None:
                yield op

def main_asm(inp, out):
    try:
        with open(inp, 'r') as inpf:
            with open(out, "wb") as outf:
                for op in progasm(inpf):
                    outf.write(op.to_bytes(4, 'big'))

    except FileNotFoundError:
        print(f"File {inp} not found.")
        return 1
    
    return 0

def opdis(code: int):
    op = code >> 28
    if op == 13:
        return ("ldi", (code >> 24) & 7, code & 0x01FF_FFFF)
    
    if op in CODES:
        abc = (code >> 6) & 7, (code >> 3) & 7, code & 7
        name, count = CODES[op]
        return (name, abc[3 - count:])
    
    return code

def main_dis(fname):
    try:
        with open(fname, 'rb') as file:
            code = file.read()
    except FileNotFoundError:
        print(f"File {fname} not found.")
        return 1
    
    for a, b, c, d in batched(code, 4):
        match opdis(a<<24 | b<<16 | c<<8 | d):
            case int(code):
                print(f"0x{code:08x} ; invalid opcode")
            case ("ldi", reg, imm):
                if 0x20 <= imm < 0x7f:
                    print(f"ldi {reg}, 0x{imm:02x} ; '{chr(imm)}'")
                else:
                    print(f"ldi {reg}, 0x{imm:02x}")
            case (name, regs):
                print(f"{name} {', '.join(map(str, regs))}")

    return 0

def main(*argv):
    match argv:
        case ["asm", inp, out]: return main_asm(inp, out)
        case ["dis", inp]: return main_dis(inp)

        case _:
            print("Usage: python vm.py <command> <filename>")
            return 1

if __name__ == "__main__":
    import sys
    main(*sys.argv[1:])