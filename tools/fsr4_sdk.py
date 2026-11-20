"""AMD's FSR SDK (FidelityFX SDK), pinned: the signed Vulkan DLLs of FSR 4 and their licence, for runtime/vk_fsr4.c
(upscaling = fsr4, Windows).

    .venv\\Scripts\\python tools\\fsr4_sdk.py

Downloads amd_fidelityfx_loader_vk.dll, amd_fidelityfx_upscaler_vk.dll and the SDK's licence once from the SDK's
repository at the pinned commit into toolchain/release-dl/fsr4-<commit>/ (the download cache of tools/make_release.py),
each checked against its SHA-256, and copies them into toolchain/fsr4/. tools/build_game.py then copies the two DLLs next to
astralchain.exe; tools/make_release.py ships them in the release's bin/ with the licence in licenses/. Nothing of the
SDK goes into the repository: AMD ships FSR 4 only as these signed binaries (no source), and its licence lets them be
redistributed in binary form with its notice. vk_fsr4.c declares the few FSR API types it uses itself.
"""
import hashlib
import shutil
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAIN_ROOT = (ROOT / 'build').resolve().parent   # toolchain/ is a junction in a worktree
REPO = 'GPUOpen-LibrariesAndSDKs/FidelityFX-SDK'
VERSION = '2.4.0'   # FSR Upscaling 4.1.2
COMMIT = '7539ae8b6568ad29ed78a753404351daf3b5cca7'   # tag v2.4.0
SIGNEDBIN = 'Kits/FidelityFX/signedbin'
# path in the repository: (name in DEST, sha256)
FILES = {
    f'{SIGNEDBIN}/amd_fidelityfx_loader_vk.dll':
        ('amd_fidelityfx_loader_vk.dll', '38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9'),
    f'{SIGNEDBIN}/amd_fidelityfx_upscaler_vk.dll':
        ('amd_fidelityfx_upscaler_vk.dll', '4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7'),
    'docs/license.md': ('LICENSE.txt', '410f9c138e766e846acf2dc93db768198def0293a017d7e3296815b451688dbe'),
}
DLLS = ('amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll')
DEST = MAIN_ROOT / 'toolchain' / 'fsr4'
BASE = f'https://github.com/{REPO}/raw/{COMMIT}'


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def fetch():
    """The pinned files in toolchain/release-dl/fsr4-<commit>/, each downloaded when missing or not the pinned file
    (github.com/<repo>/raw/ serves the DLLs themselves, not their Git LFS pointers)."""
    dl = MAIN_ROOT / 'toolchain' / 'release-dl' / f'fsr4-{COMMIT[:12]}'
    dl.mkdir(parents=True, exist_ok=True)
    for member, (name, want) in FILES.items():
        path = dl / name
        if path.is_file() and sha256_file(path) == want:
            continue
        url = f'{BASE}/{member}'
        print(f'download {url}')
        part = path.with_name(path.name + '.part')
        with urllib.request.urlopen(url, timeout=120) as r, open(part, 'wb') as f:
            shutil.copyfileobj(r, f)
        got = sha256_file(part)
        if got != want:
            part.unlink()
            sys.exit(f'{url}: sha256 {got}, pinned {want}')
        part.replace(path)
    return dl


def extract(src, dest=DEST):
    """The pinned files from the download cache into dest; returns dest."""
    dest.mkdir(parents=True, exist_ok=True)
    for name, _ in FILES.values():
        shutil.copy2(src / name, dest / name)
    return dest


def main():
    dest = extract(fetch())
    print(f'{dest}: FSR SDK {VERSION} (FSR 4.1.2); tools/build_game.py copies its DLLs next to astralchain.exe')


if __name__ == '__main__':
    main()
