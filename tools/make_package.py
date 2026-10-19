"""Builds the package for users: dist/S2FM_Mod_Pack_<version>/ and dist/S2FM_Mod_Pack_<version>.zip.

    python tools/make_package.py

Takes the built files (build_core.cmd, build_reshade.cmd, tools/reshade_icon.py first), the installer, the readme,
the license and the third party licenses. Fails if a file is missing or anything other than the listed files would
go in.
"""
import os
import re
import shutil
import sys
import time
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPS = os.path.join(REPO, "third_party", "reshade", "deps")


def version():
    text = open(os.path.join(REPO, "package", "version.h"), encoding="utf-8").read()
    return re.search(r'S2FM_MODS_VERSION "([^"]+)"', text).group(1)


# Package path: source path in the repository.
FILES = [
    ("Install.bat", "installer/Install.bat"),
    ("Uninstall.bat", "installer/Uninstall.bat"),
    ("README.txt", "installer/README.txt"),
    ("LICENSE.txt", "LICENSE.txt"),
    ("files/s2fm_mods.ps1", "installer/s2fm_mods.ps1"),
    ("files/dxgi.dll", "bin/core/dxgi.dll"),
    ("files/tools/s2fm_reshade_core.dll", "reshade/bin/s2fm_reshade_core.dll"),
    ("files/tools/s2fm_reshade.dll", "reshade/bin/s2fm_reshade.dll"),
    ("files/images/appicon.ico", "reshade/tool/images/appicon.ico"),
    ("files/images/appicon.png", "reshade/tool/images/appicon.png"),
]


def fpng_license():
    text = open(os.path.join(DEPS, "fpng", "src", "fpng.cpp"), encoding="utf-8", errors="replace").read()
    start = text.index("This is free and unencumbered software")
    end = text.index("*/", start)
    return "\n".join(line.strip() for line in text[start:end].strip().splitlines())


def notices():
    parts = [
        ("ReShade 6.8.0 (modified)", "https://github.com/crosire/reshade",
         open(os.path.join(REPO, "third_party", "reshade", "LICENSE.md"), encoding="utf-8").read()),
        ("MinHook, with the Hacker Disassembler Engine", "https://github.com/TsudaKageyu/minhook",
         open(os.path.join(REPO, "minhook", "LICENSE.txt"), encoding="utf-8-sig").read()),
        ("stb (used by ReShade)", "https://github.com/nothings/stb",
         open(os.path.join(DEPS, "stb", "LICENSE"), encoding="utf-8").read()),
        ("fpng (used by ReShade)", "https://github.com/richgel999/fpng", fpng_license()),
        ("JPEG XL simple lossless encoder (used by ReShade)", "https://github.com/libjxl/libjxl",
         open(os.path.join(DEPS, "jxl_simple_lossless", "LICENSE"), encoding="utf-8").read()),
        ("UTF8-CPP (used by ReShade)", "https://github.com/nemtrif/utfcpp",
         open(os.path.join(DEPS, "utfcpp", "LICENSE"), encoding="utf-8").read()),
    ]
    out = ["Third party licenses", "====================", "",
           "S2FM Mod Pack contains the following software. Their licenses follow.", "",
           "The ReShade panel uses the Qt 4.8 libraries (QtCore4.dll, QtGui4.dll) that come with the game. They are",
           "not included in this package. Qt is available under the GNU LGPL 2.1: https://www.qt.io", ""]
    for name, url, text in parts:
        out += ["-" * 78, name, url, "-" * 78, "", text.strip(), ""]
    return "\r\n".join(out).replace("\r\r\n", "\r\n").replace("\n", "\r\n").replace("\r\r\n", "\r\n")


def main():
    ver = version()
    name = "S2FM_Mod_Pack_" + ver
    dist = os.path.join(REPO, "dist")
    out = os.path.join(dist, name)
    missing = [src for _, src in FILES if not os.path.exists(os.path.join(REPO, src))]
    if missing:
        sys.exit("missing (build first): " + ", ".join(missing))
    shutil.rmtree(out, ignore_errors=True)
    for dst, src in FILES:
        target = os.path.join(out, dst)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copy2(os.path.join(REPO, src), target)
        if src.endswith(".dll"):
            age = (time.time() - os.path.getmtime(os.path.join(REPO, src))) / 3600.0
            print("%-36s built %.1f h ago" % (dst, age))
    with open(os.path.join(out, "THIRD_PARTY_NOTICES.txt"), "w", encoding="utf-8", newline="") as f:
        f.write(notices())
    # Nothing but the listed files.
    listed = set(d.replace("/", os.sep) for d, _ in FILES) | {"THIRD_PARTY_NOTICES.txt"}
    for root, _, files in os.walk(out):
        for fn in files:
            rel = os.path.relpath(os.path.join(root, fn), out)
            if rel not in listed:
                sys.exit("unexpected file in the package: " + rel)
    # The symbols of this build, kept beside the package (not in it): needed to read the minidumps users send.
    symbols = os.path.join(dist, name + "_symbols")
    shutil.rmtree(symbols, ignore_errors=True)
    os.makedirs(symbols)
    for pdb in ("bin/core/dxgi.pdb", "reshade/bin/s2fm_reshade_core.pdb", "reshade/bin/s2fm_reshade.pdb"):
        shutil.copy2(os.path.join(REPO, pdb), symbols)
    for dst, src in FILES:
        if src.endswith(".dll"):
            shutil.copy2(os.path.join(REPO, src), symbols)
    print("symbols kept in %s" % symbols)
    zpath = os.path.join(dist, name + ".zip")
    if os.path.exists(zpath):
        os.remove(zpath)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for root, _, files in os.walk(out):
            for fn in files:
                full = os.path.join(root, fn)
                z.write(full, os.path.join(name, os.path.relpath(full, out)))
    print("package: %s (%d KB)" % (zpath, os.path.getsize(zpath) // 1024))


if __name__ == "__main__":
    main()
