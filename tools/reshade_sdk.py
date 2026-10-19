"""Fetches ReShade v6.8.0 (crosire/reshade, BSD-3) and the submodules our build needs into
third_party/reshade, then applies reshade/patches/*.patch in name order. Idempotent: a tree at the
right commit with the same patches applied is left alone (third_party/reshade/.s2fm_patched lists them and
their sizes, so an edited patch is applied again from a clean tree).

    python tools/reshade_sdk.py
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DST = os.path.join(ROOT, "third_party", "reshade")
TAG = "v6.8.0"
COMMIT = "18deaa5"
SUBMODULES = ["deps/stb", "deps/utfcpp", "deps/fpng", "deps/jxl_simple_lossless"]
PATCHES = os.path.join(ROOT, "reshade", "patches")


def git(*args, cwd=DST):
    subprocess.check_call(["git"] + list(args), cwd=cwd)


def main():
    if not os.path.isdir(os.path.join(DST, ".git")):
        os.makedirs(os.path.dirname(DST), exist_ok=True)
        subprocess.check_call(["git", "-c", "advice.detachedHead=false", "clone", "--depth", "1", "--branch", TAG,
                               "https://github.com/crosire/reshade.git", DST])
    head = subprocess.check_output(["git", "rev-parse", "--short=7", "HEAD"], cwd=DST).decode().strip()
    if head != COMMIT:
        sys.exit("third_party/reshade is at %s, expected %s (%s)" % (head, COMMIT, TAG))
    for sm in SUBMODULES:
        if not os.listdir(os.path.join(DST, sm)) if os.path.isdir(os.path.join(DST, sm)) else True:
            git("submodule", "update", "--init", "--depth", "1", sm)
    patches = sorted(p for p in os.listdir(PATCHES) if p.endswith(".patch")) if os.path.isdir(PATCHES) else []
    want = ["%s %d" % (p, os.path.getsize(os.path.join(PATCHES, p))) for p in patches]
    marker = os.path.join(DST, ".s2fm_patched")
    done = open(marker).read().split("\n") if os.path.exists(marker) else []
    done = [d for d in done if d]
    if done != want:
        # From a clean tree: whatever was applied before is undone, files the patches added removed.
        git("checkout", "--", ".")
        git("clean", "-fdq", "--", "source", "include", "res")
        for p in patches:
            git("apply", "--whitespace=nowarn", os.path.join(PATCHES, p))
        with open(marker, "w") as f:
            f.write("\n".join(want) + "\n")
    print("ReShade %s ready in %s, patches: %s" % (TAG, DST, ", ".join(patches) or "none"))


if __name__ == "__main__":
    main()
