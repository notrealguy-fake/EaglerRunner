"""Build the application icon from the graded dark background.

Usage:  python tools/make_icon.py

Writes src/app.ico (multi-size, embedded into the exe by src/app.rc) and
resources/icon-preview.png so the result can be eyeballed.
"""

from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "resources" / "bg-dark.png"

# Square crop around the lantern-lit tower in the night scene.
CROP_BOX = (150, 250, 950, 1050)  # left, top, right, bottom
SIZES = [256, 128, 64, 48, 32, 24, 16]


def main():
    image = Image.open(SOURCE).convert("RGB")
    box = (max(0, CROP_BOX[0]), max(0, CROP_BOX[1]),
           min(image.width, CROP_BOX[2]), min(image.height, CROP_BOX[3]))
    tile = image.crop(box)

    # Square it up, then render a slightly punchier version of the art.
    side = min(tile.size)
    left = (tile.width - side) // 2
    top = (tile.height - side) // 2
    tile = tile.crop((left, top, left + side, top + side)).resize((512, 512), Image.LANCZOS)

    # Rounded-rectangle "app tile" look with a soft dark border.
    mask = Image.new("L", (512, 512), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, 511, 511), radius=76, fill=255)
    mask = mask.filter(ImageFilter.GaussianBlur(1.2))

    background = Image.new("RGB", (512, 512), (14, 14, 18))
    tile = Image.composite(tile, background, mask)

    tile.save(ROOT / "resources" / "icon-preview.png", "PNG", optimize=True)
    tile.resize((256, 256), Image.LANCZOS).save(
        ROOT / "src" / "app.ico", "ICO", sizes=[(s, s) for s in SIZES])
    print("wrote src/app.ico and resources/icon-preview.png")


if __name__ == "__main__":
    main()
