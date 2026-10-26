"""UI Automation으로 촬영한 실제 창을 같은 배율의 비교 GIF로 묶는다."""

import argparse
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def read_frames(folder):
    with (folder / "manifest.json").open(encoding="utf-8-sig") as stream:
        rows = json.load(stream)
    return rows if isinstance(rows, list) else [rows]


def render(rows, title, panel_width=704, height=1020):
    canvas = Image.new("RGB", (panel_width, height), "#11181d")
    draw = ImageDraw.Draw(canvas)
    font = ImageFont.truetype("C:/Windows/Fonts/malgun.ttf", 24)
    small = ImageFont.truetype("C:/Windows/Fonts/malgun.ttf", 18)
    draw.text((28, 18), title, font=font, fill="#a8e4d6")
    draw.text((28, 58), rows["step"], font=small, fill="#e7eff0")
    with Image.open(rows["file"]) as source:
        # 같은 DPI의 두 창에 같은 축척을 적용해 실제 크기 차이를 유지한다.
        factor = min(0.70, (panel_width - 40) / source.width, (height - 124) / source.height)
        bitmap = source.convert("RGB").resize(
            (round(source.width * factor), round(source.height * factor)), Image.Resampling.LANCZOS
        )
        canvas.paste(bitmap, ((panel_width - bitmap.width) // 2, 108))
    return canvas


def save_gif(frames, durations, path):
    frames[0].save(
        path, save_all=True, append_images=frames[1:], duration=durations,
        loop=0, disposal=2, optimize=True
    )
    with Image.open(path) as result:
        if result.n_frames < 2:
            raise RuntimeError(f"애니메이션 프레임이 없습니다: {path}")
        print(f"{path}: {result.n_frames} frames, {path.stat().st_size:,} bytes")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--editor", type=Path)
    parser.add_argument("--recording", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    before = read_frames(args.before / "frames")
    after = read_frames(args.after / "frames")
    if len(before) != len(after):
        raise RuntimeError("전후 촬영 단계 수가 다릅니다.")
    for folder, rows in ((args.before, before), (args.after, after)):
        if (folder / "saved" / "manifest.json").exists():
            rows.extend(read_frames(folder / "saved"))
    comparisons, before_frames, after_frames, durations = [], [], [], []
    for old, new in zip(before, after, strict=True):
        left = render(old, "BEFORE · 기존 UI")
        right = render(new, "AFTER · 최신 luil UI")
        combined = Image.new("RGB", (left.width + right.width + 2, left.height), "#53666c")
        combined.paste(left, (0, 0))
        combined.paste(right, (left.width + 2, 0))
        comparisons.append(combined)
        before_frames.append(left)
        after_frames.append(right)
        durations.append(max(old["duration"], new["duration"]))
    save_gif(comparisons, durations, args.output / "ux-comparison.gif")
    save_gif(before_frames, durations, args.output / "ux-before.gif")
    save_gif(after_frames, durations, args.output / "ux-after.gif")
    comparisons[15].save(args.output / "ux-comparison.png")
    if args.editor:
        rows = read_frames(args.editor)
        frames = [render(row, "편집 · 입력 보호 → 변경 확인 → 저장", 1120, 960) for row in rows]
        save_gif(frames, [row["duration"] for row in rows], args.output / "ux-editor-flow.gif")
    if args.recording:
        rows = read_frames(args.recording)
        frames = [render(row, "촬영·저장·다음 녹화", 760, 480) for row in rows]
        save_gif(frames, [row["duration"] for row in rows], args.output / "ux-recording-flow.gif")


if __name__ == "__main__":
    main()
