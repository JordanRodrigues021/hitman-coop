"""Check the mod's code patterns against a game executable, without starting the game.

  python tools/check-patterns.py <HITMAN3.exe> [source files...]

Every string literal in the sources that looks like a byte pattern ("48 8B ?? 05") is searched for
in the executable's code section. The patches need exactly one match (FindUnique); a few patterns
are searched inside a smaller range or may match several times by design (FindAll), so 0 or 2+
matches are listed, not failed.
"""
import glob, os, re, sys
import pefile

DEFAULT = ["plugin/Src/CrashGuards.cpp", "plugin/Src/GamePause.cpp", "plugin/Src/OnlineWatch.cpp", "plugin/Src/SdkWorld.cpp",
           "plugin/Src/HitmanCoop.cpp", "plugin/Src/PoseHook.cpp", "plugin/Src/GameTweaks.cpp", "plugin/Src/WorldScript.cpp"]
PATTERN = re.compile(r'"((?:[0-9A-Fa-f]{2}|\?\?)(?: (?:[0-9A-Fa-f]{2}|\?\?)){5,})"')


def compile_pattern(text):
    out = b""
    for tok in text.split():
        out += b"[\\x00-\\xff]" if tok == "??" else re.escape(bytes([int(tok, 16)]))
    return re.compile(out, re.DOTALL)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    pe = pefile.PE(sys.argv[1], fast_load=True)
    text = [s for s in pe.sections if s.Name.startswith(b".text")][0]
    code = pe.get_memory_mapped_image()[text.VirtualAddress:text.VirtualAddress + text.Misc_VirtualSize]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    files = sys.argv[2:] or [os.path.join(root, f) for f in DEFAULT]
    bad = 0
    for path in files:
        if not os.path.exists(path):
            continue
        src = open(path, encoding="utf-8", errors="replace").read()
        for m in PATTERN.finditer(src):
            line = src.count("\n", 0, m.start()) + 1
            name = re.search(r'"([^"]+)"\s*,\s*$', src[max(0, m.start() - 60):m.start()])
            hits = [h.start() + text.VirtualAddress for h in compile_pattern(m.group(1)).finditer(code)]
            label = name.group(1) if name else ""
            where = ", ".join("+0x%x" % h for h in hits[:4])
            print("%-22s %s:%d  %d match%s %s" % (label[:22], os.path.basename(path), line, len(hits), "" if len(hits) == 1 else "es", where))
            bad += len(hits) != 1
    print("%d pattern(s) without exactly one match" % bad)
    return 0


if __name__ == "__main__":
    sys.exit(main())
