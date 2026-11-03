#!/usr/bin/env python3
"""Copy the host fonts the Mac uses for the Switch shared fonts into the data folder, for the iOS build.

runtime/src/hle_pl.cpp answers nn::pl's shared font requests (types 0 to 5) with <data root>/fonts/shared_<type>.ttf,
else <data root>/fonts/shared.ttf, else a host font: $NX_SHARED_FONT, then on macOS the first of Arial Unicode and
Arial that exists. The iOS build has no host font to fall back to, so for each type the Mac serves from a host font
this copies that same file to fonts/shared_<type>.ttf, and the phone renders what the Mac renders:

    collect_host_fonts.py [--data DIR]

DIR defaults to private/wonder, the Mac's data root; its fonts/ folder then goes to the app's Documents. Type 5 (the
Nintendo extension with the button icons) is skipped: make_button_font.py makes shared_5.ttf. Types the data folder
already covers, with its own shared_<type>.ttf or with shared.ttf, are left alone, since the phone gets those files
as they are; delete a shared_<type>.ttf to copy the host font again. A file hle_pl.cpp skips (not a single font)
covers nothing, but it is reported rather than overwritten.

The outputs are copies of macOS's own fonts, so they are for local use only and must never be committed.
"""
import argparse
import os
import shutil
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_DATA = os.path.join(REPO, "private", "wonder")

# hle_pl.cpp's type names and its macOS candidates, in its order: keep both in step with it.
TYPES = {
    0: "standard",
    1: "simplified Chinese",
    2: "simplified Chinese extension",
    3: "traditional Chinese",
    4: "Korean",
    5: "Nintendo extension",
}
BUTTON_FONT_TYPE = 5
MAC_FONTS = [
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
]


def single_font(path):
    """Whether path is one TrueType or OpenType font, the check hle_pl.cpp makes (a .ttc collection is not)."""
    try:
        with open(path, "rb") as f:
            head = f.read(12)  # hle_pl.cpp also wants the whole sfnt header, not just the version tag
        return len(head) == 12 and head[:4] in (b"\x00\x01\x00\x00", b"true", b"OTTO")
    except OSError:
        return False


def host_font():
    """The host font hle_pl.cpp falls back to on this Mac, or None."""
    env = os.environ.get("NX_SHARED_FONT")
    for path in ([env] if env else []) + MAC_FONTS:
        if single_font(path):
            return path
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--data", default=DEFAULT_DATA, help="the data root (default: private/wonder)")
    args = ap.parse_args()

    if sys.platform != "darwin":
        sys.exit("this copies macOS's fonts: run it on the Mac")
    fonts = os.path.join(args.data, "fonts")
    shared = os.path.join(fonts, "shared.ttf")
    # hle_pl.cpp skips a file that is not a single font, on the Mac and on the phone alike: only a usable one covers.
    if single_font(shared):
        print(f"{shared} serves every type without its own file, on the Mac and on the phone alike: nothing to copy")
        return
    if os.path.exists(shared):
        print(f"{shared} is not a single TrueType or OpenType font: both builds skip it")
    src = host_font()
    if src is None:
        sys.exit("no host font found: " + ", ".join(MAC_FONTS))

    os.makedirs(fonts, exist_ok=True)
    for t, name in TYPES.items():
        dst = os.path.join(fonts, f"shared_{t}.ttf")
        if single_font(dst):
            print(f"type {t} ({name}): kept {dst}, which the Mac uses too")
        elif os.path.exists(dst):
            print(f"type {t} ({name}): {dst} is not a single TrueType or OpenType font, so both builds skip it; "
                  "delete it to copy the host font")
        elif t == BUTTON_FONT_TYPE:
            print(f"type {t} ({name}): skipped; make_button_font.py makes {dst}, and until it does the phone has no "
                  f"font for this type while the Mac falls back to {src}")
        else:
            shutil.copyfile(src, dst)
            print(f"type {t} ({name}): wrote {dst} <- {src} ({os.path.getsize(dst)} bytes)")


if __name__ == "__main__":
    main()
