"""Turn "hitmancoop.dll+0x8f9db" style addresses from a crash report into function names.

  python tools/symbolize.py <HitmanCoop.dll> <HitmanCoop.pdb> 0x8f9db [0x...]
  python tools/symbolize.py <HitmanCoop.dll> <HitmanCoop.pdb> --crash HitmanCoop-crash.txt
  python tools/symbolize.py <HitmanCoop.dll> <HitmanCoop.pdb> --export HitmanCoop.sym

--export writes the function table the mod ships beside its DLL ("rva name" lines, sorted): the
crash reporter names the mod's own frames from it, on players' PCs, without the symbol file.
"""
import ctypes, os, re, sys
from ctypes import wintypes

MAX_NAME = 1024


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", wintypes.ULONG), ("TypeIndex", wintypes.ULONG),
        ("Reserved", ctypes.c_ulonglong * 2), ("Index", wintypes.ULONG),
        ("Size", wintypes.ULONG), ("ModBase", ctypes.c_ulonglong),
        ("Flags", wintypes.ULONG), ("Value", ctypes.c_ulonglong),
        ("Address", ctypes.c_ulonglong), ("Register", wintypes.ULONG),
        ("Scope", wintypes.ULONG), ("Tag", wintypes.ULONG),
        ("NameLen", wintypes.ULONG), ("MaxNameLen", wintypes.ULONG),
        ("Name", ctypes.c_char * MAX_NAME),
    ]


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", wintypes.DWORD), ("Key", ctypes.c_void_p),
        ("LineNumber", wintypes.DWORD), ("FileName", ctypes.c_char_p),
        ("Address", ctypes.c_ulonglong),
    ]


class Symbols:
    BASE = 0x180000000

    def __init__(self, dll, pdb):
        self.h = ctypes.c_void_p(0x1234)
        self.d = ctypes.WinDLL("dbghelp")
        self.d.SymSetOptions(0x10 | 0x2)  # LOAD_LINES | UNDNAME
        self.d.SymInitialize.argtypes = [ctypes.c_void_p, ctypes.c_char_p, wintypes.BOOL]
        if not self.d.SymInitialize(self.h, os.path.dirname(os.path.abspath(pdb)).encode(), False):
            raise OSError("SymInitialize failed")
        self.d.SymLoadModuleEx.restype = ctypes.c_ulonglong
        self.d.SymLoadModuleEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
                                           ctypes.c_ulonglong, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
        if not self.d.SymLoadModuleEx(self.h, None, os.path.abspath(pdb).encode(), None, self.BASE,
                                      os.path.getsize(dll), None, 0):
            raise OSError("SymLoadModuleEx failed: %d" % ctypes.GetLastError())
        self.d.SymFromAddr.argtypes = [ctypes.c_void_p, ctypes.c_ulonglong,
                                       ctypes.POINTER(ctypes.c_ulonglong), ctypes.POINTER(SYMBOL_INFO)]
        self.d.SymGetLineFromAddr64.argtypes = [ctypes.c_void_p, ctypes.c_ulonglong,
                                                ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(IMAGEHLP_LINE64)]

    def functions(self):
        """(rva, name) of every function, sorted."""
        found = {}
        proto = ctypes.WINFUNCTYPE(wintypes.BOOL, ctypes.POINTER(SYMBOL_INFO), wintypes.ULONG, ctypes.c_void_p)

        def each(info, size, ctx):
            s = info.contents
            if s.Tag in (5, 10) and s.Address >= self.BASE:  # function, public symbol
                rva = s.Address - self.BASE
                name = ctypes.string_at(ctypes.addressof(s) + SYMBOL_INFO.Name.offset, s.NameLen).decode(errors="replace")
                if s.Tag == 5 or rva not in found:
                    found[rva] = name
            return True

        self.d.SymEnumSymbols.argtypes = [ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_char_p, proto, ctypes.c_void_p]
        if not self.d.SymEnumSymbols(self.h, self.BASE, b"*", proto(each), None):
            raise OSError("SymEnumSymbols failed: %d" % ctypes.GetLastError())
        return sorted(found.items())

    def name(self, rva):
        s = SYMBOL_INFO()
        s.SizeOfStruct = ctypes.sizeof(SYMBOL_INFO) - MAX_NAME
        s.MaxNameLen = MAX_NAME
        disp = ctypes.c_ulonglong(0)
        out = "?"
        if self.d.SymFromAddr(self.h, self.BASE + rva, ctypes.byref(disp), ctypes.byref(s)):
            out = "%s+0x%x" % (s.Name.decode(errors="replace"), disp.value)
        line = IMAGEHLP_LINE64()
        line.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINE64)
        d2 = wintypes.DWORD(0)
        if self.d.SymGetLineFromAddr64(self.h, self.BASE + rva, ctypes.byref(d2), ctypes.byref(line)):
            out += "  (%s:%d)" % (os.path.basename(line.FileName.decode(errors="replace")), line.LineNumber)
        return out


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    sym = Symbols(sys.argv[1], sys.argv[2])
    if sys.argv[3] == "--export":
        funcs = sym.functions()
        with open(sys.argv[4], "w", encoding="ascii", errors="replace", newline="\n") as out:
            for rva, name in funcs:
                # Short enough for a line of the report; templates' arguments say little there.
                out.write("%x %s\n" % (rva, name[:140].replace("\n", " ")))
        print("%d functions -> %s" % (len(funcs), sys.argv[4]))
        return 0 if len(funcs) > 500 else 1
    if sys.argv[3] == "--crash":
        for line in open(sys.argv[4], encoding="utf-8", errors="replace"):
            m = re.search(r"(?i)hitmancoop\.dll\+0x([0-9a-f]+)", line)
            print(line.rstrip()[:60] + ("   -> " + sym.name(int(m.group(1), 16)) if m else ""))
    else:
        for a in sys.argv[3:]:
            print(a, "->", sym.name(int(a, 16)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
