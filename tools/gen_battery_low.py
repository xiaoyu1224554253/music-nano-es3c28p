import os
from PIL import Image, ImageDraw

W, H = 172, 320

BLACK = (0, 0, 0)
WHITE = (255, 255, 255)
RED   = (220, 30, 30)


def draw_image():
    img = Image.new("RGB", (W, H), BLACK)
    d = ImageDraw.Draw(img)

    bw, bh = 64, 136
    bx = (W - bw) // 2
    by = (H - bh) // 2
    border = 4

    cap_w = 20
    cap_h = 8
    cap_x = bx + (bw - cap_w) // 2
    cap_y = by - cap_h
    d.rectangle([cap_x, cap_y, cap_x + cap_w, cap_y + cap_h], fill=WHITE)
    d.rounded_rectangle([bx, by, bx + bw, by + bh], radius=8, outline=WHITE, width=border)

    ix0 = bx + border
    ix1 = bx + bw - border
    iy0 = by + border
    iy1 = by + bh - border

    fill_h = 18
    d.rectangle([ix0, iy1 - fill_h, ix1, iy1], fill=RED)
    return img


def rgb_to_rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def img_to_rgb565_bytes(img):
    out = bytearray(W * H * 2)
    px = img.load()
    for y in range(H):
        for x in range(W):
            r, g, b = px[x, y][:3]
            v = rgb_to_rgb565(r, g, b)
            i = (y * W + x) * 2
            out[i] = (v >> 8) & 0xFF
            out[i + 1] = v & 0xFF
    return bytes(out)


def emit_header(data):
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.normpath(os.path.join(here, "..", "main", "resources", "battery_low_img.h"))
    lines = []
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append(f"#define BATTERY_LOW_IMG_W {W}")
    lines.append(f"#define BATTERY_LOW_IMG_H {H}")
    lines.append("")
    lines.append("static const uint8_t battery_low_img[BATTERY_LOW_IMG_W * BATTERY_LOW_IMG_H * 2] = {")
    per_row = 16
    for i in range(0, len(data), per_row):
        chunk = data[i:i + per_row]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ("," if i + per_row < len(data) else ""))
    lines.append("};")
    lines.append("")
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"Saved: {out} ({len(data)} bytes)")


img = draw_image()
here = os.path.dirname(os.path.abspath(__file__))
img.save(os.path.join(here, "battery_low_preview.png"))
print(f"Saved: {os.path.join(here, 'battery_low_preview.png')} ({W}x{H})")
emit_header(img_to_rgb565_bytes(img))
