"""Disassemble the game around an address from a crash report (HITMAN3.exe+0x...).

  python tools/gamedis.py <HITMAN3.exe> func 0x174eb05          the whole function holding it
  python tools/gamedis.py <HITMAN3.exe> at 0x174eb05 [before] [after]   bytes around it
  python tools/gamedis.py <HITMAN3.exe> start 0x174eb05         only the function's bounds
"""
import bisect, struct, sys
import capstone, pefile


class Game:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.data = self.pe.get_memory_mapped_image()
        d = self.pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]  # exception directory
        raw = self.data[d.VirtualAddress:d.VirtualAddress + d.Size]
        self.funcs = sorted(struct.unpack_from("<III", raw, i)[:2] for i in range(0, len(raw) - 11, 12))
        self.starts = [f[0] for f in self.funcs]
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

    def func(self, rva):
        i = bisect.bisect_right(self.starts, rva) - 1
        if i >= 0 and self.funcs[i][0] <= rva < self.funcs[i][1]:
            return self.funcs[i]
        return None

    def dis(self, start, end, mark=None):
        out = []
        for ins in self.md.disasm(bytes(self.data[start:end]), start):
            out.append("%s %8x  %-8s %s" % (">>" if ins.address == mark else "  ", ins.address, ins.mnemonic, ins.op_str))
        return out


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    g = Game(sys.argv[1])
    rva = int(sys.argv[3], 16)
    f = g.func(rva)
    if sys.argv[2] == "start":
        print("function %x..%x (%d bytes)" % (f[0], f[1], f[1] - f[0]) if f else "not in a function")
    elif sys.argv[2] == "func":
        if not f:
            print("not in a function")
            return 1
        print("function %x..%x" % f)
        print("\n".join(g.dis(f[0], f[1], rva)))
    else:
        before = int(sys.argv[4]) if len(sys.argv) > 4 else 64
        after = int(sys.argv[5]) if len(sys.argv) > 5 else 32
        start = max(f[0], rva - before) if f else rva - before
        # Walk from the function's start so the instructions line up.
        lines = g.dis(f[0] if f else start, rva + after, rva)
        print("function %x..%x" % f if f else "no function")
        print("\n".join(l for l in lines if int(l[3:11], 16) >= start))
    return 0


if __name__ == "__main__":
    sys.exit(main())
