"""ffx-vk-linux, vendored in runtime/third_party/ffx-vk-linux (MIT): libffx_vk_linux.so, which runs AMD's FSR 4 Vulkan
DLLs in the Linux game (runtime/vk_fsr4.c, upscaling = fsr4). Linux only: on Windows the game loads AMD's DLLs itself.

    .venv/bin/python tools/ffx_vk_linux.py [--out DIR]

AMD publishes FSR 4 as signed Windows DLLs only (amd_fidelityfx_loader_vk.dll and amd_fidelityfx_upscaler_vk.dll of the
FSR SDK 2.4.0; tools/fsr4_sdk.py pins and fetches them). ffx-vk-linux (https://github.com/DuIslingr/ffx-vk-linux, commit
e1d5a415ed78dd5db7c6d00258d06acccb424307) loads those DLLs in a native Linux process: a PE loader, the Windows services
and the calling-convention bridges they need, and AMD's FSR API exported under its own names. Its src/, include/, the
FSR SDK's API headers it carries (third_party/ffx, MIT) and tools/gen_vk_thunks.py are in the repository unmodified; its
probes and tests are not.

build() compiles them into <out>/libffx_vk_linux.so the way the project's CMakeLists.txt does: the Vulkan thunks
generated from the registry's vk.xml, C++20, hidden visibility, linked with libcrypto (the Authenticode check of the
DLLs), SPIRV-Tools (fixes to the DLLs' shaders) and libdrm_amdgpu (the GPU family the DLLs ask for). tools/build_game.py
calls it for a Linux developer build and puts the library next to astralchain with AMD's two DLLs (toolchain/fsr4);
tools/make_release_linux.py calls it with the release's clang, sysroot and a static OpenSSL, and ships the library and
the DLLs in bin/, from where the installer copies them: the player supplies nothing.

Needs, beyond the game's toolchain: the development files of OpenSSL 3 or later, libdrm, SPIRV-Tools and the SPIR-V
headers, and the Vulkan registry (Fedora: openssl-devel libdrm-devel spirv-tools-devel spirv-headers-devel
vulkan-headers; Debian: libssl-dev libdrm-dev spirv-tools spirv-headers libvulkan-dev). When one is missing build() says
which and returns None: the game then builds without the library, and with upscaling = fsr4 it says which file is
missing and runs FSR 3 (vk_fsr4.c).
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / 'runtime' / 'third_party' / 'ffx-vk-linux'
LIB = 'libffx_vk_linux.so'
SOURCES = ('pe.cpp', 'teb.cpp', 'win32.cpp', 'win32_runtime.cpp', 'win32_security.cpp', 'adapter.cpp',
           'spirv_images.cpp', 'vk_thunks.cpp', 'ffxvk.cpp')
INCLUDES = ('include', 'third_party/ffx/api/include', 'third_party/ffx/upscalers/include', 'src')
LINK = ('-ldl', '-lpthread', '-lcrypto', '-lSPIRV-Tools', '-ldrm_amdgpu', '-ldrm')
# the headers of the libraries above, as the sources include them: what the check below compiles
PROBE = ('#include <openssl/core_names.h>\n#include <openssl/pkcs7.h>\n#include <amdgpu.h>\n#include <xf86drm.h>\n'
         '#include <spirv-tools/libspirv.h>\n#include <spirv/unified1/spirv.hpp11>\n#include <vulkan/vulkan.h>\n')
VK_XML = ('/usr/share/vulkan/registry/vk.xml', '/usr/local/share/vulkan/registry/vk.xml')


def drm_cflags():
    """libdrm's include directory (amdgpu.h and xf86drm.h include <drm.h> from it)."""
    pc = shutil.which('pkg-config')
    if pc:
        r = subprocess.run([pc, '--cflags', 'libdrm'], capture_output=True, text=True)
        if r.returncode == 0:
            return r.stdout.split()
    return ['-I/usr/include/libdrm']


def vk_xml():
    """The Vulkan registry: $VULKAN_SDK's, else the system's. None when there is none."""
    sdk = os.environ.get('VULKAN_SDK')
    for p in ([f'{sdk}/share/vulkan/registry/vk.xml'] if sdk else []) + list(VK_XML):
        if Path(p).is_file():
            return Path(p)
    return None


def stamp(cxx, flags, xml):
    """What a built library depends on: every vendored file, the registry, the compiler and its flags."""
    h = hashlib.sha256()
    for p in sorted(SRC.rglob('*')):
        if p.is_file():
            h.update(p.relative_to(SRC).as_posix().encode() + b'\0' + p.read_bytes())
    h.update(xml.read_bytes())
    h.update(' '.join([str(cxx), *flags]).encode())
    return h.hexdigest()


def build(out, cxx=None, flags=None, link=LINK, xml=None, quiet=False):
    """<out>/libffx_vk_linux.so, compiled when it is not there or its inputs changed; objects go into <out>/ffx-vk-linux/.
    cxx: the C++ compiler (clang++ on PATH); flags: its flags for the libraries' headers (the host's libdrm; a release
    gives its sysroot and include directories); link: the libraries; xml: the Vulkan registry (the host's). Returns the
    library's path, or None with one line on stderr when a prerequisite is missing."""
    def skip(why):
        print(f'ffx_vk_linux: {why}: {LIB} is not built (FSR 4 off, FSR 3 runs in its place)', file=sys.stderr)

    out = Path(out)
    cxx = cxx or shutil.which('clang++') or shutil.which('g++')
    if not cxx:
        return skip('no C++ compiler (clang++) on PATH')
    xml = xml or vk_xml()
    if not xml:
        return skip('the Vulkan registry (vk.xml of the Vulkan headers) is not installed')
    flags = list(drm_cflags() if flags is None else flags)
    probe = subprocess.run([str(cxx), '-std=c++20', *flags, '-fsyntax-only', '-x', 'c++', '-'], input=PROBE,
                           capture_output=True, text=True)
    if probe.returncode:
        first = next((line for line in probe.stderr.splitlines() if 'error' in line), probe.stderr.strip()[:200])
        return skip(f'a development header is missing ({first.strip()}); see tools/ffx_vk_linux.py for the packages')
    work = out / 'ffx-vk-linux'
    lib, mark = out / LIB, work / '.built'
    want = stamp(cxx, [*flags, *map(str, link)], xml)
    if lib.is_file() and mark.is_file() and mark.read_text(encoding='utf-8').strip() == want:
        return lib
    work.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, str(SRC / 'tools' / 'gen_vk_thunks.py'), str(xml), str(work / 'vk_thunks.inc')],
                   check=True)
    common = [str(cxx), '-std=c++20', '-O2', '-fPIC', '-fvisibility=hidden', '-Wall', '-Wno-attributes',
              '-Wno-deprecated-declarations', *flags, *[f'-I{SRC / d}' for d in INCLUDES], f'-I{work}']
    jobs = [subprocess.Popen([*common, '-c', str(SRC / 'src' / s), '-o', str(work / (s[:-4] + '.o'))]) for s in SOURCES]
    if any(j.wait() for j in jobs):
        sys.exit(f'ffx_vk_linux: compiling {SRC} failed')
    objects = [str(work / (s[:-4] + '.o')) for s in SOURCES]
    linked = subprocess.run([str(cxx), '-shared', *flags, '-o', str(lib), *objects, *map(str, link)])
    if linked.returncode:
        sys.exit(f'ffx_vk_linux: linking {lib} failed (libcrypto, SPIRV-Tools, libdrm and libdrm_amdgpu are linked)')
    mark.write_text(want + '\n', encoding='utf-8')
    if not quiet:
        print(f'{lib}: ffx-vk-linux built ({lib.stat().st_size >> 10} KiB)')
    return lib


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'ffx-vk-linux', help='where the library goes')
    ap.add_argument('--cxx', help='the C++ compiler (default: clang++ on PATH)')
    a = ap.parse_args()
    if not sys.platform.startswith('linux'):
        sys.exit('ffx_vk_linux: Linux only; on Windows the game loads AMD\'s FSR 4 DLLs itself (tools/fsr4_sdk.py)')
    sys.exit(0 if build(a.out, a.cxx) else 1)


if __name__ == '__main__':
    main()
