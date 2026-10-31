# Checks a packaged release: the mod DLL and connection test inside the zip and inside the setup
# exe's embedded payload are the ones just built, and say the version; the .sha256 files match;
# update.txt names this version and the updates branch, and every file it lists is the
# package's own (size, SHA-256) and is in the update folder.
# Usage: python -I tools/check-release.py <version>   (after tools/package.ps1)
import hashlib, io, os, struct, sys, zipfile
v = sys.argv[1]
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dist = os.path.join(root, '_dist')
built = {
    'HitmanCoop.dll': open(os.path.join(root, '_build', 'plugin', 'RelWithDebInfo', 'HitmanCoop.dll'), 'rb').read(),
    'Test-Connection.exe': open(os.path.join(root, '_build', 'install', 'HitmanCoop-NetTest.exe'), 'rb').read(),
}
sha = lambda b: hashlib.sha256(b).hexdigest()
ok = True

def check(label, z):
    global ok
    names = z.namelist()
    for want, data in built.items():
        hits = [n for n in names if n.replace('\\', '/').endswith('/' + want) or n == want]
        if len(hits) != 1:
            print(label, want, 'MISSING or duplicated', hits); ok = False; continue
        got = z.read(hits[0])
        same = sha(got) == sha(data)
        has_v = v.encode() in got
        print(f'{label}: {hits[0]} same-as-build={same} says-{v}={has_v}')
        ok &= same and has_v

check('zip', zipfile.ZipFile(rf'{dist}\HitmanCoop-{v}.zip'))

exe = open(rf'{dist}\HitmanCoop-Setup-{v}.exe', 'rb').read()
eocd = exe.rfind(b'PK\x05\x06')
cd_size, cd_off = struct.unpack('<II', exe[eocd + 12:eocd + 20])
start = eocd - cd_size - cd_off
check('setup payload', zipfile.ZipFile(io.BytesIO(exe[start:eocd + 22])))

for f in (rf'HitmanCoop-Setup-{v}.exe', rf'HitmanCoop-{v}.zip'):
    data = open(rf'{dist}\{f}', 'rb').read()
    listed = open(rf'{dist}\{f}.sha256', 'rb').read()
    good = listed.split()[0].decode() == sha(data) and b'\r' not in listed
    print(f, 'sha256 file matches (LF only):', good)
    ok &= good

upd = rf'{dist}\update-{v}'
text = open(os.path.join(upd, 'update.txt'), 'rb').read()
lines = text.decode('utf-8').split('\n')
base_ok = 'base https://raw.githubusercontent.com/JordanRodrigues021/hitman-coop/updates/' in lines
ver_ok = f'version {v}' in lines
print('update.txt: version', ver_ok, '| base is the updates branch', base_ok, '| CR-free', b'\r' not in text)
ok &= base_ok and ver_ok and b'\r' not in text
pkg_files = rf'{dist}\HitmanCoop-{v}\files'
listed = 0
for line in lines:
    if not line.startswith('file '):
        if line == 'notes':
            break
        continue
    _, path, size, digest = line.split()
    listed += 1
    pkg = open(os.path.join(pkg_files, path.replace('/', '\\')), 'rb').read()
    asset = open(os.path.join(upd, path.split('/')[-1]), 'rb').read()
    good = len(pkg) == int(size) and sha(pkg) == digest and asset == pkg
    if path == 'mods/HitmanCoop.dll':
        good &= sha(pkg) == sha(built['HitmanCoop.dll'])
    print(f'  update file {path}: matches package{" and build" if path.endswith("HitmanCoop.dll") else ""} {good}')
    ok &= good
in_pkg = sum(len(fs) for _, _, fs in os.walk(pkg_files))
print('update.txt lists every packaged game file:', listed == in_pkg, f'({listed} of {in_pkg})')
ok &= listed == in_pkg
print('ALL OK' if ok else 'PROBLEM')
