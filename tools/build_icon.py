from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"
SIZE = 1024


def interpolate(start: tuple[int, int, int], end: tuple[int, int, int], amount: float):
    return tuple(round(a + (b - a) * amount) for a, b in zip(start, end))


def build_icon() -> Image.Image:
    scale = 4
    canvas_size = SIZE * scale
    image = Image.new("RGBA", (canvas_size, canvas_size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    start = (37, 99, 235)
    end = (6, 182, 212)
    for y in range(canvas_size):
        color = (*interpolate(start, end, y / (canvas_size - 1)), 255)
        draw.line((0, y, canvas_size, y), fill=color)

    mask = Image.new("L", image.size, 0)
    mask_draw = ImageDraw.Draw(mask)
    mask_draw.rounded_rectangle(
        (48 * scale, 48 * scale, 976 * scale, 976 * scale),
        radius=232 * scale,
        fill=255,
    )
    image.putalpha(mask)

    draw = ImageDraw.Draw(image)
    white = (255, 255, 255, 255)
    width = 46 * scale
    draw.rounded_rectangle(
        (216 * scale, 264 * scale, 448 * scale, 760 * scale),
        radius=56 * scale,
        outline=white,
        width=width,
    )
    draw.ellipse((318 * scale, 674 * scale, 346 * scale, 702 * scale), fill=white)
    draw.rounded_rectangle(
        (584 * scale, 308 * scale, 844 * scale, 656 * scale),
        radius=40 * scale,
        outline=white,
        width=width,
    )
    draw.line(
        (714 * scale, 656 * scale, 714 * scale, 756 * scale),
        fill=white,
        width=width,
    )
    draw.line(
        (650 * scale, 756 * scale, 778 * scale, 756 * scale),
        fill=white,
        width=width,
    )
    for y in (432, 512, 592):
        draw.arc(
            (438 * scale, (y - 54) * scale, 594 * scale, (y + 54) * scale),
            start=210,
            end=330,
            fill=white,
            width=30 * scale,
        )

    return image.resize((SIZE, SIZE), Image.Resampling.LANCZOS)


def main() -> None:
    ASSETS.mkdir(exist_ok=True)
    icon = build_icon()
    icon.save(ASSETS / "voice-bridge.png")
    icon.save(
        ASSETS / "voice-bridge.ico",
        sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)],
    )


if __name__ == "__main__":
    main()
