"""SC5 Online's own pictures: the emblem, the window and file icons, and the ranked titles' badges.

    python launcher/tools/make_art.py            # writes launcher/data/logo and launcher/data/ranks

Everything is drawn here from shapes (Pillow): a sword through a gold V in a ring for the emblem, a shield for each
rank, its material rising with the ladder (bronze, silver, gold, a blue "soul" crystal), crossed swords, a crown and
wings for the higher ones, and diamonds for the step within each material. Nothing comes from the game or its
publisher. The titles are the server's (server/elo.py RANKS), the files named as the launcher looks them up
(style::rankKey: lower case, dashes for spaces). Same output on every run.
"""
import math
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = pathlib.Path(__file__).resolve().parent
DATA = HERE.parent / "data"
SS = 4  # supersampling

RANKS = ["Novice", "Apprentice", "Squire", "Wanderer", "Duelist", "Knight", "Vanguard", "Blademaster",
         "Champion", "Conqueror", "Hero", "Legend", "Soul Warrior", "Soul Master", "Spirit Sword", "Eternal Soul"]

GOLD = ((0x8C, 0x64, 0x1C), (0xF6, 0xDA, 0x8E))
BLUE = ((0x18, 0x45, 0x9C), (0xB4, 0xDA, 0xFF))
MATERIALS = [
    ((0x6E, 0x3F, 0x1A), (0xD9, 0x9A, 0x5A)),  # bronze
    ((0x6F, 0x7A, 0x8A), (0xEE, 0xF2, 0xF7)),  # silver
    GOLD,                                       # gold
    BLUE,                                       # soul crystal
]


def key(title):
    out = ""
    for c in title.strip().lower():
        if c.isalnum():
            out += c
        elif c in " -" and out and not out.endswith("-"):
            out += "-"
    return out.rstrip("-")


def font(size):
    for name in ("palab.ttf", "georgiab.ttf", "timesbd.ttf", "DejaVuSerif-Bold.ttf"):
        for folder in (pathlib.Path("C:/Windows/Fonts"), pathlib.Path("/usr/share/fonts/truetype/dejavu")):
            f = folder / name
            if f.exists():
                return ImageFont.truetype(str(f), size)
    return ImageFont.load_default()


def gradient(size, top, bottom, horizontal=False):
    w, h = size
    g = Image.new("RGBA", size)
    px = g.load()
    n = w if horizontal else h
    for i in range(n):
        a = i / max(1, n - 1)
        c = tuple(int(top[k] + (bottom[k] - top[k]) * a) for k in range(3)) + (255,)
        if horizontal:
            for y in range(h):
                px[i, y] = c
        else:
            for x in range(w):
                px[x, i] = c
    return g


def fill(canvas, shape_mask, top, bottom, box=None, horizontal=False):
    """paint the mask's shape with a gradient over its bounding box"""
    bbox = box or shape_mask.getbbox()
    if not bbox:
        return
    x0, y0, x1, y1 = bbox
    g = gradient((x1 - x0, y1 - y0), top, bottom, horizontal)
    layer = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    layer.paste(g, (x0, y0))
    canvas.alpha_composite(Image.composite(layer, Image.new("RGBA", canvas.size, (0, 0, 0, 0)), shape_mask))


def polygon_mask(size, pts):
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).polygon(pts, fill=255)
    return m


def bezier(p0, p1, p2, n=24):
    return [((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t * t * p2[0],
             (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t * t * p2[1]) for t in (i / n for i in range(n + 1))]


def shield(cx, top, w, h):
    x0, x1 = cx - w / 2, cx + w / 2
    shoulder = top + h * 0.42
    pts = [(x0, top + h * 0.04), (cx, top - h * 0.03), (x1, top + h * 0.04), (x1, shoulder)]
    pts += bezier((x1, shoulder), (x1, top + h * 0.82), (cx, top + h))[1:]
    pts += bezier((cx, top + h), (x0, top + h * 0.82), (x0, shoulder))[1:]
    return pts


def sword(size, cx, top, length, width, blade, hilt, angle=0.0):
    """a sword pointing down, hilt at the top, turned by angle degrees around its middle"""
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    grip_h, guard_h = length * 0.16, length * 0.05
    blade_top = top + grip_h + guard_h
    tip = top + length
    bw = width
    blade_pts = [(cx - bw / 2, blade_top), (cx + bw / 2, blade_top), (cx + bw / 2, tip - bw * 1.6), (cx, tip),
                 (cx - bw / 2, tip - bw * 1.6)]
    fill(layer, polygon_mask(size, blade_pts), blade[1], blade[0], horizontal=False)
    # a bright line down the blade's middle
    d = ImageDraw.Draw(layer)
    d.line([(cx, blade_top + bw * 0.4), (cx, tip - bw * 1.2)], fill=(255, 255, 255, 150), width=max(1, int(bw * 0.18)))
    guard_w = bw * 4.2
    guard = [(cx - guard_w / 2, top + grip_h), (cx + guard_w / 2, top + grip_h),
             (cx + guard_w / 2 - bw * 0.4, top + grip_h + guard_h), (cx - guard_w / 2 + bw * 0.4, top + grip_h + guard_h)]
    fill(layer, polygon_mask(size, guard), hilt[1], hilt[0])
    grip = [(cx - bw * 0.32, top + bw * 0.6), (cx + bw * 0.32, top + bw * 0.6), (cx + bw * 0.32, top + grip_h),
            (cx - bw * 0.32, top + grip_h)]
    fill(layer, polygon_mask(size, grip), (0x3A, 0x2A, 0x1A), (0x1C, 0x14, 0x0C))
    r = bw * 0.62
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).ellipse([cx - r, top - r * 0.2, cx + r, top + r * 1.8], fill=255)
    fill(layer, m, hilt[1], hilt[0])
    if angle:
        layer = layer.rotate(angle, resample=Image.BICUBIC, center=(cx, top + length / 2))
    return layer


def emblem(px):
    """the SC5 Online emblem: a gold ring, a navy field, a gold V and a blue sword through it"""
    s = px * SS
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    c = s / 2
    ring = Image.new("L", (s, s), 0)
    ImageDraw.Draw(ring).ellipse([s * 0.02, s * 0.02, s * 0.98, s * 0.98], fill=255)
    fill(im, ring, GOLD[1], GOLD[0])
    field = Image.new("L", (s, s), 0)
    ImageDraw.Draw(field).ellipse([s * 0.075, s * 0.075, s * 0.925, s * 0.925], fill=255)
    fill(im, field, (0x16, 0x2C, 0x58), (0x05, 0x08, 0x10))
    # a thin inner gold line
    ImageDraw.Draw(im).ellipse([s * 0.105, s * 0.105, s * 0.895, s * 0.895], outline=(0xC9, 0x9E, 0x45, 255),
                               width=max(1, int(s * 0.008)))
    # the V
    f = font(int(s * 0.62))
    layer = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    mask = Image.new("L", (s, s), 0)
    md = ImageDraw.Draw(mask)
    l, t, r, b = md.textbbox((0, 0), "V", font=f)
    md.text((c - (l + r) / 2, c - (t + b) / 2 + s * 0.035), "V", font=f, fill=255)
    fill(layer, mask, GOLD[1], GOLD[0])
    im.alpha_composite(layer)
    # a glow behind the blade, then the sword
    glow = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    ImageDraw.Draw(glow).rectangle([c - s * 0.035, s * 0.2, c + s * 0.035, s * 0.86], fill=(0x6F, 0xB0, 0xFF, 150))
    im.alpha_composite(glow.filter(ImageFilter.GaussianBlur(s * 0.03)))
    im.alpha_composite(sword((s, s), c, s * 0.12, s * 0.76, s * 0.05, BLUE, GOLD))
    return im.resize((px, px), Image.LANCZOS)


def wings(size, cx, cy, span, height, colors, feathers):
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    for side in (-1, 1):
        for i in range(feathers):
            a = i / max(1, feathers - 1)
            length = span * (1.0 - 0.45 * a)
            angle = math.radians(18 - 34 * a)  # upper feathers long and raised, lower ones short and dropped
            w = height * 0.16
            x0 = cx + side * span * 0.08
            y0 = cy - height * 0.22 + a * height * 0.48
            x1 = x0 + side * length * math.cos(angle)
            y1 = y0 - length * math.sin(angle)
            nx, ny = -(y1 - y0), (x1 - x0)
            n = math.hypot(nx, ny) or 1
            nx, ny = nx / n * w / 2, ny / n * w / 2
            pts = [(x0 + nx, y0 + ny), ((x0 + x1) / 2 + nx * 1.3, (y0 + y1) / 2 + ny * 1.3), (x1, y1),
                   ((x0 + x1) / 2 - nx * 1.3, (y0 + y1) / 2 - ny * 1.3), (x0 - nx, y0 - ny)]
            m = polygon_mask(size, pts)
            fill(layer, m, colors[1], colors[0], horizontal=True)
            ImageDraw.Draw(layer).line(pts + [pts[0]], fill=(0, 0, 0, 90), width=max(1, int(height * 0.012)))
    return layer


def crown(size, cx, base_y, w, h, colors):
    pts = [(cx - w / 2, base_y), (cx - w / 2, base_y - h * 0.55), (cx - w / 4, base_y - h * 0.25), (cx, base_y - h),
           (cx + w / 4, base_y - h * 0.25), (cx + w / 2, base_y - h * 0.55), (cx + w / 2, base_y)]
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    fill(layer, polygon_mask(size, pts), colors[1], colors[0])
    d = ImageDraw.Draw(layer)
    d.line(pts + [pts[0]], fill=(0x3A, 0x28, 0x08, 255), width=max(1, int(h * 0.06)))
    for x, y in ((cx - w / 2, base_y - h * 0.55), (cx, base_y - h), (cx + w / 2, base_y - h * 0.55)):
        r = h * 0.11
        d.ellipse([x - r, y - r, x + r, y + r], fill=(0xFF, 0xF1, 0xC4, 255))
    return layer


def badge(index):
    """rank `index` of 16: material index // 4, step index % 4 + 1"""
    material, step = index // 4, index % 4 + 1
    W, H = 480 * SS, 200 * SS
    size = (W, H)
    im = Image.new("RGBA", size, (0, 0, 0, 0))
    cx = W / 2
    sh_w, sh_h, sh_top = H * 0.50, H * 0.62, H * 0.13
    col = MATERIALS[material]
    if material >= 2:  # wings behind the shield
        span = W * (0.24 if material == 2 else 0.36)
        wcol = GOLD if material == 2 else BLUE
        im.alpha_composite(wings(size, cx, sh_top + sh_h * 0.42, span, H * 0.62, wcol, 4 if material == 2 else 6))
    if material == 3:  # the soul's glow
        glow = Image.new("RGBA", size, (0, 0, 0, 0))
        ImageDraw.Draw(glow).ellipse([cx - sh_w * 0.75, sh_top - H * 0.06, cx + sh_w * 0.75, sh_top + sh_h * 1.08],
                                     fill=(0x6F, 0xB0, 0xFF, 120 + 25 * step))
        im.alpha_composite(glow.filter(ImageFilter.GaussianBlur(H * 0.06)))
    pts = shield(cx, sh_top, sh_w, sh_h)
    # rim, then the field inside it
    fill(im, polygon_mask(size, pts), col[1], col[0])
    inner = shield(cx, sh_top + sh_h * 0.07, sh_w * 0.82, sh_h * 0.83)
    field_top = (0x1A, 0x2E, 0x55) if material < 3 else (0x10, 0x2C, 0x66)
    fill(im, polygon_mask(size, inner), field_top, (0x06, 0x0A, 0x14))
    ImageDraw.Draw(im).line(pts + [pts[0]], fill=(0, 0, 0, 160), width=max(1, int(H * 0.012)))
    # the arms: one sword, then crossed swords
    blade = BLUE if material == 3 else ((0x8F, 0x9C, 0xAE), (0xF4, 0xF7, 0xFB))
    hilt = GOLD if material >= 1 else col
    length = sh_h * 0.74
    if material == 0:
        im.alpha_composite(sword(size, cx, sh_top + sh_h * 0.12, length, sh_w * 0.075, blade, hilt))
    else:
        for a in (-32, 32):
            im.alpha_composite(sword(size, cx, sh_top + sh_h * 0.1, length, sh_w * 0.068, blade, hilt, angle=a))
    if material >= 2:
        im.alpha_composite(crown(size, cx, sh_top + H * 0.005, sh_w * 0.42, H * 0.14, GOLD))
    # the step within the material: 1 to 4 diamonds under the shield
    d = ImageDraw.Draw(im)
    r = H * 0.045
    gap = r * 2.7
    y = sh_top + sh_h + H * 0.1
    x0 = cx - gap * (step - 1) / 2
    for i in range(step):
        x = x0 + i * gap
        dia = [(x, y - r), (x + r * 0.8, y), (x, y + r), (x - r * 0.8, y)]
        fill(im, polygon_mask(size, dia), col[1], col[0])
        d.line(dia + [dia[0]], fill=(0, 0, 0, 170), width=max(1, int(H * 0.008)))
    return im.resize((480, 200), Image.LANCZOS)


def main():
    logo = DATA / "logo"
    ranks = DATA / "ranks"
    logo.mkdir(parents=True, exist_ok=True)
    ranks.mkdir(parents=True, exist_ok=True)
    emblem(320).save(logo / "sc5-logo.png", optimize=True)
    icons = {}
    for n in (16, 24, 32, 48, 64, 128, 256):
        icons[n] = emblem(n)
        icons[n].save(logo / f"icon-{n}.png", optimize=True)
    icons[256].save(logo / "sc5.ico", sizes=[(n, n) for n in (16, 24, 32, 48, 64, 128, 256)])
    for i, title in enumerate(RANKS):
        badge(i).save(ranks / f"{key(title)}.png", optimize=True)
    print(f"wrote {logo} and {len(RANKS)} badges in {ranks}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
