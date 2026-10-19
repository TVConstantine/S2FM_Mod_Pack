"""The package's Install.bat and Uninstall.bat on a made-up game, on another drive. Runs outside the game and never
touches the real one.

    python tests/installer_test.py

A free drive letter is mapped (subst) to obj/installer_test/drive, with a Steam folder whose libraryfolders.vdf
names a second library with a space in its name, and a game there holding a dxgi.dll of someone else's and a copy of
the real sdkenginetools.txt. The package zip is unpacked into a folder with spaces. Then: install, install again,
uninstall, each through the .bat files. Judged: our files go in, the other dxgi.dll is kept and put back, the tools
list gets one entry and is byte for byte the same after uninstall, nothing of ours is left. The drive is unmapped
at the end.
"""
import os
import shutil
import subprocess
import sys
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(REPO, "obj", "installer_test")
REAL_TOOLS = r"C:\Program Files (x86)\Steam\steamapps\common\Half-Life Alyx\game\bin\sdkenginetools.txt"
OTHER_DXGI = b"MZ someone else's dxgi.dll"
failed = 0


def check(what, ok):
    global failed
    print("%s  %s" % ("PASS" if ok else "FAIL", what))
    if not ok:
        failed += 1


def free_letter():
    for c in "XWVUTSRQPONMLKJ":
        if not os.path.exists(c + ":\\"):
            return c
    sys.exit("no free drive letter")


def run_bat(bat, *args):
    # The .bat ends with "pause": a line on its input answers it.
    p = subprocess.run(["cmd", "/c", bat] + list(args), input="\n", capture_output=True, text=True, cwd=os.path.dirname(bat))
    out = p.stdout.strip()
    print("  " + out.replace("\n", "\n  "))
    return out


def read(p):
    return open(p, "rb").read() if os.path.exists(p) else None


def main():
    subprocess.run([sys.executable, os.path.join(REPO, "tools", "make_package.py")], check=True)
    dist = os.path.join(REPO, "dist")
    zips = sorted((f for f in os.listdir(dist) if f.endswith(".zip")), key=lambda f: os.path.getmtime(os.path.join(dist, f)))
    shutil.rmtree(WORK, ignore_errors=True)
    drive_dir = os.path.join(WORK, "drive")
    os.makedirs(drive_dir)
    letter = free_letter()
    subprocess.run(["subst", letter + ":", drive_dir], check=True)
    try:
        root = letter + ":\\"
        steam = root + "Steam"
        lib = root + "Games Lib"
        game = lib + r"\steamapps\common\Half-Life Alyx"
        os.makedirs(steam + r"\steamapps")
        with open(steam + r"\steamapps\libraryfolders.vdf", "w") as f:
            f.write('"libraryfolders"\n{\n\t"0"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n\t"1"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n}\n'
                    % (steam.replace("\\", "\\\\"), lib.replace("\\", "\\\\")))
        os.makedirs(game + r"\game\bin\win64")
        os.makedirs(game + r"\game\core\tools\images")
        open(lib + r"\steamapps\appmanifest_546560.acf", "w").write('"AppState"\n{\n}\n')
        open(game + r"\game\bin\win64\dxgi.dll", "wb").write(OTHER_DXGI)
        # The tools list as a new user has it: the real one without our entry (TOOLS_WITH_ENTRY=1 keeps it, as on a
        # machine where it was there before).
        real = open(REAL_TOOLS, "rb").read().decode("utf-8")
        if not os.environ.get("TOOLS_WITH_ENTRY") and 'm_Name = "s2fm_reshade"' in real:
            t = real.replace("\r\n", "\n")
            name = t.find('m_Name = "s2fm_reshade"')
            begin = t.rfind("\n", 0, t.rfind("{", 0, name)) + 1
            end = t.find("\n", t.find("},", name)) + 1
            t = t[:begin] + t[end:]
            real = t.replace("\n", "\r\n") if "\r\n" in real else t
        print("the made-up tools list %s our entry" % ("has" if 'm_Name = "s2fm_reshade"' in real else "lacks"))
        open(game + r"\game\bin\sdkenginetools.txt", "wb").write(real.encode("utf-8"))
        tools_before = read(game + r"\game\bin\sdkenginetools.txt")

        pkg_dir = root + "My Downloads\\S2FM Mod Pack test"
        with zipfile.ZipFile(os.path.join(REPO, "dist", zips[-1])) as z:
            z.extractall(pkg_dir)
        pkg = os.path.join(pkg_dir, os.listdir(pkg_dir)[0])
        print("package at %s, game at %s" % (pkg, game))

        out = run_bat(os.path.join(pkg, "Install.bat"), "-SteamDir", steam)
        check("the game is found in the second library, on drive %s:" % letter, ("game folder: " + game).lower() in out.lower())
        ours = {r"game\bin\win64\dxgi.dll": r"files\dxgi.dll",
                r"game\bin\win64\tools\s2fm_reshade_core.dll": r"files\tools\s2fm_reshade_core.dll",
                r"game\bin\win64\tools\s2fm_reshade.dll": r"files\tools\s2fm_reshade.dll",
                r"game\core\tools\images\s2fm_reshade\appicon.ico": r"files\images\appicon.ico",
                r"game\core\tools\images\s2fm_reshade\appicon.png": r"files\images\appicon.png"}
        check("our files are in", all(read(os.path.join(game, d)) == read(os.path.join(pkg, s)) for d, s in ours.items()))
        check("the other dxgi.dll is kept", read(game + r"\game\bin\win64\s2fm_mods_backup\game\bin\win64\dxgi.dll") == OTHER_DXGI)
        tools = read(game + r"\game\bin\sdkenginetools.txt").decode("utf-8")
        check("the tools list has the panel once", tools.count('m_Name = "s2fm_reshade"') == 1)

        run_bat(os.path.join(pkg, "Install.bat"), "-SteamDir", steam)
        tools = read(game + r"\game\bin\sdkenginetools.txt").decode("utf-8")
        check("installing again: still once in the tools list", tools.count('m_Name = "s2fm_reshade"') == 1)
        check("installing again: the kept dxgi.dll is still the other one",
              read(game + r"\game\bin\win64\s2fm_mods_backup\game\bin\win64\dxgi.dll") == OTHER_DXGI)

        open(game + r"\game\bin\win64\s2fm_mods.log", "w").write("made while running")
        run_bat(os.path.join(pkg, "Uninstall.bat"), "-SteamDir", steam)
        check("the other dxgi.dll is back", read(game + r"\game\bin\win64\dxgi.dll") == OTHER_DXGI)
        check("the tools list is as it was, byte for byte", read(game + r"\game\bin\sdkenginetools.txt") == tools_before)
        left = [d for d in list(ours) + [r"game\bin\win64\s2fm_mods_backup", r"game\bin\win64\s2fm_mods.log",
                                          r"game\core\tools\images\s2fm_reshade"]
                if d != r"game\bin\win64\dxgi.dll" and os.path.exists(os.path.join(game, d))]
        check("nothing of ours is left (%s)" % left, not left)
    finally:
        subprocess.run(["subst", letter + ":", "/d"])
    print("%s, %d failed" % ("OK" if not failed else "FAILED", failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
