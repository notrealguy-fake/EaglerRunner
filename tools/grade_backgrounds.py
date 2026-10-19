"""Enhance and color grade the launcher backgrounds.

Usage:  python tools/grade_backgrounds.py

Reads  background_dark.jpg  and  background_light.webp  from the project root and
writes resources/bg-dark.png and resources/bg-light.png (PNG, as requested).

The pipeline per image: downscale -> light denoise -> local contrast (clarity) ->
split toning -> lift/gamma/gain -> S-curve contrast -> vibrance -> bloom -> vignette
-> unsharp mask.
"""

from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
TARGET_WIDTH = 1920


# ----------------------------------------------------------------- helpers ----
def to_float(image):
    return np.asarray(image.convert("RGB"), dtype=np.float32) / 255.0


def to_image(array):
    return Image.fromarray(np.clip(array * 255.0 + 0.5, 0, 255).astype(np.uint8), "RGB")


def blur(array, radius):
    return to_float(to_image(array).filter(ImageFilter.GaussianBlur(radius)))


def luma(array):
    return array[..., 0] * 0.2126 + array[..., 1] * 0.7152 + array[..., 2] * 0.0722


def denoise(array, radius, mix):
    return array * (1.0 - mix) + blur(array, radius) * mix


def clarity(array, amount, radius):
    """Local contrast: push detail relative to a blurred copy."""
    return array + amount * (array - blur(array, radius))


def split_tone(array, shadow_tint, highlight_tint, shadow_amount, highlight_amount):
    lightness = luma(array)[..., None]
    shadows = np.clip(1.0 - lightness / 0.5, 0, 1) ** 1.4
    highlights = np.clip((lightness - 0.5) / 0.5, 0, 1) ** 1.4
    return (array
            + np.asarray(shadow_tint, dtype=np.float32) * shadows * shadow_amount
            + np.asarray(highlight_tint, dtype=np.float32) * highlights * highlight_amount)


def lift_gamma_gain(array, lift, gamma, gain):
    x = np.clip(array * gain + lift, 0, 1)
    return np.power(x, 1.0 / max(gamma, 1e-6))


def contrast_curve(array, amount):
    return np.clip(0.5 + (array - 0.5) * amount, 0, 1)


def vibrance(array, saturation, vibrance_amount):
    lightness = luma(array)[..., None]
    out = lightness + (array - lightness) * (1.0 + saturation)
    if vibrance_amount:
        high = out.max(-1)
        low = out.min(-1)
        sat = np.where(high > 1e-5, (high - low) / np.maximum(high, 1e-5), 0.0)
        weight = (1.0 - sat)[..., None]
        out = out + (out - luma(out)[..., None]) * vibrance_amount * weight
    return out


def bloom(array, threshold, amount, radius):
    """Soft glow on bright areas (lanterns, clouds)."""
    lightness = luma(array)
    mask = np.clip((lightness - threshold) / max(1.0 - threshold, 1e-6), 0, 1)[..., None]
    glow = blur(array * mask, radius)
    return 1.0 - (1.0 - array) * (1.0 - np.clip(glow * amount, 0, 1))


def vignette(array, strength, start=0.55, power=1.6):
    height, width = array.shape[:2]
    yy, xx = np.mgrid[0:height, 0:width]
    cy, cx = (height - 1) / 2.0, (width - 1) / 2.0
    distance = np.sqrt(((yy - cy) / max(cy, 1.0)) ** 2 + ((xx - cx) / max(cx, 1.0)) ** 2)
    falloff = np.clip((distance - start) / (1.0 - start), 0, 1) ** power
    return array * (1.0 - strength * falloff[..., None])


def finish(array, sharpen_radius=1.6, sharpen_percent=90, sharpen_threshold=3):
    image = to_image(np.clip(array, 0, 1))
    return image.filter(ImageFilter.UnsharpMask(radius=sharpen_radius,
                                                percent=sharpen_percent,
                                                threshold=sharpen_threshold))


def resize_width(image, width):
    if image.width <= width:
        return image
    height = round(image.height * width / image.width)
    return image.resize((width, height), Image.LANCZOS)


# ------------------------------------------------------------------ grades ----
def grade_dark(path):
    """Night scene: keep the warm lanterns, sink the shadows to deep blue."""
    image = resize_width(Image.open(path), TARGET_WIDTH)
    a = to_float(image)

    a = denoise(a, 0.7, 0.45)                       # clean up JPEG blocking
    a = clarity(a, 0.38, 7.0)                       # punch up the masonry detail
    a = split_tone(a,                               # teal shadows / amber highlights
                   shadow_tint=(0.015, 0.045, 0.150),
                   highlight_tint=(0.105, 0.048, -0.020),
                   shadow_amount=0.40,
                   highlight_amount=0.28)
    a = lift_gamma_gain(a, lift=0.012, gamma=1.16, gain=1.10)   # open the night up
    a = contrast_curve(a, 1.10)
    a = vibrance(a, saturation=0.10, vibrance_amount=0.20)
    a = bloom(a, threshold=0.48, amount=0.44, radius=18)        # lantern glow
    a = vignette(a, strength=0.24, start=0.58)
    return finish(a, sharpen_percent=95)


def grade_light(path):
    """Day scene: airy and sunlit, richer pinks, less haze."""
    image = resize_width(Image.open(path), TARGET_WIDTH)
    a = to_float(image)

    a = denoise(a, 0.6, 0.35)
    a = clarity(a, 0.26, 7.0)                       # dehaze + foliage detail
    a = split_tone(a,                               # cool shade / warm sunlight
                   shadow_tint=(-0.010, 0.020, 0.085),
                   highlight_tint=(0.070, 0.012, 0.010),
                   shadow_amount=0.22,
                   highlight_amount=0.26)
    a = lift_gamma_gain(a, lift=0.0, gamma=1.03, gain=1.05)
    a = contrast_curve(a, 1.09)
    a = vibrance(a, saturation=0.08, vibrance_amount=0.30)      # pink blossoms pop
    a = bloom(a, threshold=0.74, amount=0.20, radius=12)
    a = vignette(a, strength=0.20, start=0.58)
    return finish(a, sharpen_percent=85)


def main():
    out_dir = ROOT / "resources"
    out_dir.mkdir(exist_ok=True)

    jobs = [
        (ROOT / "background_dark.jpg", out_dir / "bg-dark.png", grade_dark),
        (ROOT / "background_light.webp", out_dir / "bg-light.png", grade_light),
    ]
    for source, target, grader in jobs:
        if not source.exists():
            print(f"skipped (missing): {source.name}")
            continue
        result = grader(source)
        result.save(target, "PNG", optimize=True)
        size_mb = target.stat().st_size / (1024 * 1024)
        print(f"{target.relative_to(ROOT)}  {result.width}x{result.height}  {size_mb:.2f} MB")


if __name__ == "__main__":
    main()
