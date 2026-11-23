#!/usr/bin/env python3
# tiramisu/tools/gen-assets.py — generate the full UWP asset set from the
# REAL branding committed in package/branding/ (no placeholder glyphs).
#
# Sources:
#   package/branding/logo.png   1254x1254 — the Tiramisu logo
#   package/branding/banner.jpg 3584x1184 — the banner art (tiramisunewlogo)
#
# Every asset the AppxManifest references is produced here at the exact
# pixel size Windows expects, cover-cropped so art never stretches.
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
LOGO = os.path.join(HERE, "..", "package", "branding", "logo.png")
BANNER = os.path.join(HERE, "..", "package", "branding", "banner.jpg")
BG = (14, 17, 22, 255)  # #0E1116 — the manifest BackgroundColor


def cover_fit(im, w, h):
    """Scale + center-crop to exactly (w, h) without distortion."""
    src_ratio = im.width / im.height
    dst_ratio = w / h
    if src_ratio > dst_ratio:      # source wider: crop sides
        new_w = int(im.height * dst_ratio)
        x = (im.width - new_w) // 2
        im = im.crop((x, 0, x + new_w, im.height))
    else:                          # source taller: crop top/bottom
        new_h = int(im.width / dst_ratio)
        y = (im.height - new_h) // 2
        im = im.crop((0, y, im.width, y + new_h))
    return im.resize((w, h), Image.LANCZOS)


def pad_fit(im, w, h, scale=0.86):
    """Logo centered on the background color, scaled to fit with margin."""
    im = im.convert("RGBA")
    inner_w, inner_h = int(w * scale), int(h * scale)
    ratio = min(inner_w / im.width, inner_h / im.height)
    im = im.resize((max(1, int(im.width * ratio)), max(1, int(im.height * ratio))),
                   Image.LANCZOS)
    out = Image.new("RGBA", (w, h), BG)
    out.paste(im, ((w - im.width) // 2, (h - im.height) // 2), im)
    return out


def fit_pad(im, w, h, bg=(255, 255, 255)):
    """Whole image scaled to fit inside (w, h), centred on bg — never cropped.

    The banner art is 3:1, so cover-cropping it into 16:9 or 2.07:1 tiles
    cuts the wordmark and the mascot off at the edges.
    """
    ratio = min(w / im.width, h / im.height)
    im = im.resize((max(1, int(im.width * ratio)), max(1, int(im.height * ratio))),
                   Image.LANCZOS)
    out = Image.new("RGB", (w, h), bg)
    out.paste(im, ((w - im.width) // 2, (h - im.height) // 2))
    return out


def save(im, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im.convert("RGB").save(path, "PNG")
    print("  wrote", path, f"{im.width}x{im.height}")


def main(out_dir):
    logo = Image.open(LOGO)
    banner = Image.open(BANNER)

    # Squares (tiles + icon): the logo on the manifest background color
    for name, (w, h) in {
        "Square44x44Logo.png": (44, 44),
        "Square150x150Logo.png": (150, 150),
        "Square310x310Logo.png": (310, 310),
        "StoreLogo.png": (50, 50),
    }.items():
        save(pad_fit(logo, w, h), os.path.join(out_dir, name))

    # Scale variants for the 44x44 (Windows picks by display scale)
    for tag, w in {"scale-100": 44, "scale-125": 55, "scale-150": 66,
                   "scale-200": 88, "scale-400": 176}.items():
        save(pad_fit(logo, w, w),
             os.path.join(out_dir, "Square44x44Logo",
                          f"Square44x44Logo-{tag}.png"))
    for ts in (16, 24, 32, 48, 256):
        save(pad_fit(logo, ts, ts),
             os.path.join(out_dir, "Square44x44Logo",
                          f"Square44x44Logo-targetsize-{ts}.png"))

    # Wide tile + splash: the banner art, whole (fit + pad, not cropped)
    save(fit_pad(banner, 310, 150),
         os.path.join(out_dir, "Wide310x150Logo.png"))
    save(fit_pad(banner, 1920, 1080),
         os.path.join(out_dir, "SplashScreen.png"))
    print("assets written to", out_dir)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "out/layout/Assets")
