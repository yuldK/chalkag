"""고정 Codicons 글꼴에서 Windows 앱 아이콘을 생성한다."""

import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


project_root = Path(__file__).resolve().parent.parent
codicons_root = project_root / "third_party" / "luil" / "assets" / "codicons"
assets_root = project_root / "assets"
icon_name = "device-camera"
icon_sizes = (16, 20, 24, 32, 40, 48, 64, 128, 256)


def render_icon(size, glyph):
    factor = 4
    dimension = size * factor
    image = Image.new("RGBA", (dimension, dimension))
    draw = ImageDraw.Draw(image)
    inset = dimension / 32
    draw.rounded_rectangle(
        (inset, inset, dimension - inset - 1, dimension - inset - 1),
        radius=dimension * 0.22,
        fill="#173D37",
    )
    font = ImageFont.truetype(str(codicons_root / "codicon.ttf"), round(dimension * 0.72))
    left, top, right, bottom = font.getbbox(glyph)
    position = ((dimension - (right - left)) / 2 - left, (dimension - (bottom - top)) / 2 - top)
    draw.text(position, glyph, font=font, fill="#69E0C4")
    return image.resize((size, size), Image.Resampling.LANCZOS)


def main():
    mapping = json.loads((codicons_root / "mapping.json").read_text(encoding="utf-8"))
    glyph = next(chr(int(codepoint)) for codepoint, names in mapping.items() if icon_name in names)
    assets_root.mkdir(exist_ok=True)
    images = [render_icon(size, glyph) for size in icon_sizes]
    images[-1].save(assets_root / "chalkag.png")
    images[-1].save(
        assets_root / "chalkag.ico",
        format="ICO",
        sizes=[(size, size) for size in icon_sizes],
        append_images=images[:-1],
    )
    with Image.open(assets_root / "chalkag.ico") as icon:
        if icon.ico.sizes() != {(size, size) for size in icon_sizes}:
            raise RuntimeError("ICO에 필요한 해상도가 모두 포함되지 않았습니다.")
    print(f"Created {icon_name} icon: {', '.join(map(str, icon_sizes))} px")


if __name__ == "__main__":
    main()
