#!/usr/bin/env python3
"""DS asset pipeline: bakes the 3DS port's already rendered assets down for the Nintendo DS.

Usage: tools/build_ds_assets.py [path to sd3d-port]

Uses the existing full-res PNGs in sd3d-port/build/assets/pc instead of re-rasterising the SVGs, so the
title logo stays exactly as it is ("Space Destroyer", no s, faithful to the original).

Writes gen/textures.bin + gen/tex_palettes.bin (VRAM images), gen/ds_tex_gen.h (costume -> texture table),
gen/ui_gen.h (bottom screen palette, fonts, button images), gen/sfx/*.wav (maxmod soundbank input),
gen/music.bin (IMA-ADPCM, embedded in the ARM9 binary), gen/music_gen.h, gen/icon.bmp.
"""
import colorsys, os, re, subprocess, struct, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(ROOT), "sd3d-port")
PC = os.path.join(PORT, "build/assets/pc")
FONTS = os.path.join(PORT, "tools/fonts")
GEN = os.path.join(ROOT, "gen")
DS = 192.0 / 360.0                 # DS pixels per stage unit: the 480x360 stage fills 256x192 exactly
TEX_BUDGET = 512 * 1024            # VRAM banks A-D
RATE = 16000                       # stage music rate, mono IMA-ADPCM (8 KB/s); thumbnail stem at half of it
EGG_RATE = 11025
MUSIC_BUDGET = 2600 * 1024         # ARM9 image must stay under about 3.4 MB to leave a healthy heap (ARM7 loads at 0x02380000)

# ---------------------------------------------------------------- costume table
src = open(os.path.join(PORT, "src/common/costumes_gen.h")).read()
COST = []
for m in re.finditer(r'\{"(\w+)", ([^}]*)\}', src):
    v = [x.strip().rstrip("f") for x in m.group(2).split(",")]
    COST.append(dict(file=m.group(1), w=float(v[0]), h=float(v[1]), rcx=float(v[2]), rcy=float(v[3]),
                     pc_scale=float(v[4]), pc_ox=float(v[5]), pc_oy=float(v[6]), pc_w=int(v[7]), pc_h=int(v[8])))
assert len(COST) == 24

# file -> mip widths in texels (POT texture widths; only used rows take VRAM). None => no texture.
WORLD_MIPS = {
    "star01": [8], "star1": [8], "star001": [16],
    "earth": [256, 128, 64, 32, 16, 8], "planet2": [256, 128, 64, 32, 16, 8],
    "powerup": [128, 64, 32, 16, 8], "bullet": [128, 64, 32, 16, 8],
}
HUES = {"powerup": 32, "bullet": 32, "togglemusic": 2}
SOLID = {"star01", "star1"}        # drawn as plain coloured quads while small

def pot(n):
    p = 8
    while p < n: p *= 2
    return p

def load_costume(c):
    p = os.path.join(PC, c["file"] + ".png")
    if not os.path.exists(p): p = os.path.join(PC, c["file"] + "_h0.png")
    im = Image.open(p).convert("RGBA").crop((0, 0, c["pc_w"], c["pc_h"]))
    return im

def resize_premult(im, size):
    a = np.asarray(im, dtype=np.float32) / 255.0
    a[..., :3] *= a[..., 3:4]
    chans = [Image.fromarray(a[..., i]).resize(size, Image.LANCZOS if max(size) >= 32 else Image.BOX) for i in range(4)]
    o = np.stack([np.asarray(x) for x in chans], -1).clip(0, 1)
    al = o[..., 3:4]
    rgb = np.where(al > 1e-3, o[..., :3] / np.maximum(al, 1e-3), 0).clip(0, 1)
    return np.concatenate([rgb, al], -1)

def scratch_hue(rgb, effect):
    """Scratch's colour effect, exactly as its shader does it"""
    if effect == 0: return rgb
    h, s, v = colorsys.rgb_to_hsv(*rgb)
    if v < 0.055: h, s, v = 0, 1, 0.055
    elif s < 0.09: h, s = 0, 0.09
    return colorsys.hsv_to_rgb((h + effect / 200.0) % 1.0, s, v)

def bgr555(rgb):
    r, g, b = [min(31, int(round(x * 31))) for x in rgb]
    return r | (g << 5) | (b << 10)

def build_palette(arrays, n=32):
    """one n-colour palette for all mips of a costume"""
    px = np.concatenate([a[a[..., 3] > 0.02][:, :3] for a in arrays])
    if len(px) == 0: px = np.zeros((1, 3))
    im = Image.fromarray((px.reshape(-1, 1, 3) * 255 + 0.5).astype(np.uint8), "RGB")
    q = im.quantize(colors=n, method=Image.MEDIANCUT, dither=Image.NONE)
    pal = np.array(q.getpalette()[:n * 3], dtype=np.float32).reshape(-1, 3) / 255.0
    return np.vstack([pal, np.zeros((n - len(pal), 3))]) if len(pal) < n else pal

def a3i5(arr, pal):
    h, w = arr.shape[:2]
    rgb = arr[..., :3].reshape(-1, 3)
    d = ((rgb[:, None, :] - pal[None, :, :]) ** 2).sum(-1)
    idx = d.argmin(1)
    al = np.rint(arr[..., 3].reshape(-1) * 7).astype(np.uint8)
    return ((al << 5) | idx.astype(np.uint8)).reshape(h, w)

# ---------------------------------------------------------------- textures
tex_blob = bytearray(); pal_blob = bytearray(); entries = []; cost_map = []
for ci, c in enumerate(COST):
    f = c["file"]
    if f == "pause_button":        # PC-only sprite (the DS uses a bottom screen button)
        cost_map.append((len(entries), 0, 0, 0, 0)); continue
    im = load_costume(c)
    cw, ch = c["pc_w"] / c["pc_scale"], c["pc_h"] / c["pc_scale"]   # costume units covered by the image
    ox, oy = c["pc_ox"], c["pc_oy"]
    if f in ("title", "changelog", "pause_panel"):
        # static full-screen art: crop to what the 480x360 stage can ever show
        x0 = max(ox, c["rcx"] - 240.0); y0 = max(oy, c["rcy"] - 180.0)
        x1 = min(ox + cw, c["rcx"] + 240.0); y1 = min(oy + ch, c["rcy"] + 180.0)
        s = c["pc_scale"]
        im = im.crop((int((x0 - ox) * s), int((y0 - oy) * s), int((x1 - ox) * s), int((y1 - oy) * s)))
        ox, oy, cw, ch = x0, y0, x1 - x0, y1 - y0
    if f in WORLD_MIPS:
        sizes = []
        for tw in WORLD_MIPS[f]:
            sizes.append((tw, tw, max(1, int(round(tw * ch / cw)))))
    else:
        tex = 0.1 if f == "dot" else 1.0        # the dot is only drawn at 10%
        uw, uh = max(1, int(round(cw * DS * tex))), max(1, int(round(ch * DS * tex)))
        sizes = [(pot(uw), uw, uh)]
    arrays = [resize_premult(im, (uw, uh)) for _, uw, uh in sizes]
    pal = build_palette(arrays)
    pal_base = len(pal_blob)
    nh = HUES.get(f, 1)
    for hi in range(nh):
        effect = hi * 200.0 / nh if f != "togglemusic" else hi * 50.0
        for col in pal:
            pal_blob += struct.pack("<H", bgr555(scratch_hue(tuple(col), effect)))
    first = len(entries)
    for (tw, uw, uh), a in zip(sizes, arrays):
        data = a3i5(a, pal)
        th = pot(uh)
        while len(tex_blob) % 8: tex_blob.append(0)
        addr = len(tex_blob)
        # a texture only needs the rows it samples: the next one may start right after them
        tex_blob += np.pad(data, ((0, 0), (0, tw - uw)), constant_values=0).tobytes()
        code_w, code_h = int(np.log2(tw)) - 3, int(np.log2(th)) - 3
        fmt = (addr >> 3) | (code_w << 20) | (code_h << 23) | (1 << 26)     # format 1 = A3I5
        entries.append((fmt, uw, uh))
    # costume-unit rect covered by the texture -> DS pixels at 100% size, q4
    off_x, off_y = (ox - c["rcx"]) * DS, (oy - c["rcy"]) * DS
    wpx, hpx = cw * DS, ch * DS
    solid = 0
    if f in SOLID:
        al = np.asarray(im, dtype=np.float32) / 255.0
        w = al[..., 3:4]
        m = (al[..., :3] * w).sum((0, 1)) / max(1e-3, w.sum())
        solid = bgr555(tuple(m / max(1e-3, m.max())))
    cost_map.append((first, len(sizes), pal_base >> 4, 64 >> 4 if nh > 1 else 0, nh, solid,
                     int(round(off_x * 16)), int(round(off_y * 16)), int(round(wpx * 16)), int(round(hpx * 16))))
    print(f"{f:14s} mips {[s[1:] for s in sizes]} A3I5 hues {nh}")
assert len(tex_blob) <= TEX_BUDGET, len(tex_blob)
print(f"textures {len(tex_blob)} bytes of {TEX_BUDGET}, palettes {len(pal_blob)} bytes")
os.makedirs(GEN, exist_ok=True)
open(os.path.join(GEN, "textures.bin"), "wb").write(tex_blob)
open(os.path.join(GEN, "tex_palettes.bin"), "wb").write(pal_blob)
with open(os.path.join(GEN, "ds_tex_gen.h"), "w") as o:
    o.write("/* Generated by tools/build_ds_assets.py - do not edit. */\n")
    o.write("typedef struct { uint32_t fmt; uint16_t uw, uh; } DsMip;\n")
    o.write("typedef struct { uint8_t first, nmips, nhues, hue_step; uint16_t pal, solid; int16_t off_x, off_y, wq4, hq4; } DsCostume;\n")
    o.write("static const DsMip ds_mips[] = {\n")
    for fmt, uw, uh in entries: o.write(f"  {{0x{fmt:08x}u, {uw}, {uh}}},\n")
    o.write("};\nstatic const DsCostume ds_costumes[CO_COUNT] = {\n")
    for t in cost_map:
        if len(t) < 10: o.write("  {0},\n"); continue
        first, nm, pal, step, nh, solid, ox, oy, w, h = t
        o.write(f"  {{{first}, {nm}, {nh}, {step}, {pal}, {solid}, {ox}, {oy}, {w}, {h}}},\n")
    o.write("};\n")

# ---------------------------------------------------------------- bottom screen: palette, fonts, buttons
COL = dict(BG=(0x1d, 0x1c, 0x29), YELLOW=(0xff, 0xe6, 0x00), DIM=(0x8c, 0x7f, 0x14), MONBG=(0xe6, 0xf0, 0xff),
           MONBRD=(0xb8, 0xbf, 0xcc), LABEL=(0x57, 0x5e, 0x75), ORANGE=(0xff, 0x8c, 0x1a), WHITE=(255, 255, 255),
           BLACK=(0, 0, 0), BLUE=(0x4c, 0x97, 0xff), TRACK=(0xc6, 0xcb, 0xd6), GREY=(0x80, 0x80, 0x90))
PAIRS = [("LABEL", "MONBG"), ("WHITE", "ORANGE"), ("YELLOW", "BG"), ("BLACK", "WHITE"), ("DIM", "BG"), ("GREY", "BG")]
pal = [COL["BG"]]; names = {"BG": 0}
for k, v in COL.items():
    if k not in names: names[k] = len(pal); pal.append(v)
ramp_base = len(pal)
for fg, bg in PAIRS:
    for t in (1 / 3, 2 / 3):
        pal.append(tuple(int(round(b + (f - b) * t)) for f, b in zip(COL[fg], COL[bg])))
UI_RESERVED = 64
pal += [(0, 0, 0)] * (UI_RESERVED - len(pal))

BTN_SCALE = 0.74 * 0.8     # 3DS bottom screen scale x 256/320
BTNS = ["play", "back", "selectmode", "hardmode", "whatsnew", "normalmode", "selected"]
images = {}
for name in BTNS + ["togglemusic", "togglemusic_h50"]:
    base = name.replace("_h50", "")
    c = next(x for x in COST if x["file"] == base)
    p = os.path.join(PC, name + ".png")
    if not os.path.exists(p): p = os.path.join(PC, name + "_h0.png")
    im = Image.open(p).convert("RGBA").crop((0, 0, c["pc_w"], c["pc_h"]))
    sc = 0.7 * 0.8 if name == "selected" else BTN_SCALE
    tw, th = int(round(c["w"] * sc)), int(round(c["h"] * sc))
    a = resize_premult(im, (tw, th))
    bg = np.array(COL["BG"], dtype=np.float32) / 255.0
    rgb = a[..., :3] * a[..., 3:4] + bg * (1 - a[..., 3:4])
    images[name] = (rgb * 255 + 0.5).astype(np.uint8)
sheet = np.concatenate([v.reshape(-1, 3) for v in images.values()]).reshape(-1, 1, 3)
q = Image.fromarray(sheet, "RGB").quantize(colors=256 - UI_RESERVED, method=Image.MEDIANCUT, dither=Image.NONE)
qp = np.array(q.getpalette()[:(256 - UI_RESERVED) * 3], dtype=np.int32).reshape(-1, 3)
for rgb in qp: pal.append(tuple(int(x) for x in rgb))
pal += [(0, 0, 0)] * (256 - len(pal))

def bake_font(name, ttf, px, fallback=None, threshold=None):
    f = ImageFont.truetype(os.path.join(FONTS, ttf), px)
    asc, desc = f.getmetrics()
    h = asc + desc
    glyphs = []
    for code in [1] + list(range(32, 127)):
        if code == 1 and fallback: ff, ch = ImageFont.truetype(os.path.join(FONTS, fallback), px), "\u2601"
        else: ff, ch = f, " " if code == 1 else chr(code)
        w = int(np.ceil(ff.getlength(ch))) or 1
        im = Image.new("L", (w + 2, h), 0)
        ImageDraw.Draw(im).text((1, 0), ch, font=ff, fill=255)
        a = np.asarray(im, dtype=np.float32) / 255.0
        if threshold is not None: lv = (a >= threshold).astype(np.uint8) * 3
        else: lv = np.rint(a * 3).astype(np.uint8)
        glyphs.append((w, lv[:, 1:w + 1]))
    return name, h, asc, glyphs

fonts = [bake_font("label", "LiberationSans-Bold.ttf", 10, "DejaVuSans.ttf"),
         bake_font("pixel", "Grand9K-Pixel.ttf", 8, None, 0.5),
         bake_font("big", "Grand9K-Pixel.ttf", 16, None, 0.5)]
with open(os.path.join(GEN, "ui_gen.h"), "w") as o:
    o.write("/* Generated by tools/build_ds_assets.py - do not edit. */\n")
    for k in COL: o.write(f"#define UI_{k} {names[k]}\n")
    for i, (fg, bg) in enumerate(PAIRS): o.write(f"#define PAIR_{fg}_{bg} {i}\n")
    o.write(f"#define UI_RAMP_BASE {ramp_base}\n")
    o.write("static const uint8_t ui_pairs[][2] = {" + ",".join(f"{{UI_{f},UI_{b}}}" for f, b in PAIRS) + "};\n")
    o.write("static const uint16_t ui_palette[256] = {" + ",".join(str(bgr555(tuple(x / 255 for x in c))) for c in pal) + "};\n")
    for name, h, asc, glyphs in fonts:
        o.write(f"static const uint8_t font_{name}_px[] = {{")
        offs = []; pos = 0
        for w, lv in glyphs:
            offs.append((pos, w)); pos += lv.size
            o.write(",".join(str(int(x)) for x in lv.flatten()) + ",")
        o.write("};\n")
        o.write(f"static const uint16_t font_{name}_tab[][2] = {{" + ",".join(f"{{{a},{b}}}" for a, b in offs) + "};\n")
        o.write(f"static const UiFont font_{name} = {{ {h}, font_{name}_px, font_{name}_tab }};\n")
    for name, arr in images.items():
        lab = np.asarray(Image.fromarray(arr, "RGB").quantize(palette=q, dither=Image.NONE), dtype=np.int32) + UI_RESERVED
        o.write(f"static const uint8_t img_{name}_px[] = {{" + ",".join(str(int(x)) for x in lab.flatten()) + "};\n")
        o.write(f"static const UiImage img_{name} = {{ {arr.shape[1]}, {arr.shape[0]}, img_{name}_px }};\n")

# ---------------------------------------------------------------- icon: 32x32, 16 colours (ndstool banner)
title = Image.open(os.path.join(PORT, "build/assets/cia/icon.png")).convert("RGB").resize((32, 32), Image.LANCZOS)
icon = title.quantize(colors=16, method=Image.MEDIANCUT, dither=Image.NONE)
ip = (icon.getpalette() + [0] * 48)[:48]
rows = np.asarray(icon, dtype=np.uint8)[::-1]             # ndstool wants a 4 bpp BMP, which PIL can't write
data = b"".join(bytes((r[i] << 4) | r[i + 1] for i in range(0, 32, 2)) for r in rows)
hdr = struct.pack("<IiiHHIIiiII", 40, 32, 32, 1, 4, 0, len(data), 0, 0, 16, 16)
pal4 = b"".join(bytes((ip[i * 3 + 2], ip[i * 3 + 1], ip[i * 3], 0)) for i in range(16))
with open(os.path.join(GEN, "icon.bmp"), "wb") as o:
    o.write(b"BM" + struct.pack("<IHHI", 14 + 40 + 64 + len(data), 0, 0, 14 + 40 + 64) + hdr + pal4 + data)

# ---------------------------------------------------------------- audio
def ffmpeg_pcm(path, rate):
    out = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", str(rate), "-f", "s16le", "-"],
                         check=True, stdout=subprocess.PIPE).stdout
    return np.frombuffer(out, dtype=np.int16)

STEP = [7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,
        253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,
        3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,
        22385,24623,27086,29794,32767]
IDX = [-1,-1,-1,-1,2,4,6,8]
IDX0 = 20                      # step index every section starts from (decoder restarts from the header constants)

def ima_codes(pcm):
    """-> list of 4-bit codes. The encoder restarts at sample[0], so a section can be entered directly."""
    pred = int(pcm[0]); idx = IDX0; out = []
    for s in pcm.tolist():
        step = STEP[idx]; d = s - pred; code = 0
        if d < 0: code = 8; d = -d
        dq = step >> 3
        if d >= step: code |= 4; d -= step; dq += step
        step >>= 1
        if d >= step: code |= 2; d -= step; dq += step
        step >>= 1
        if d >= step: code |= 1; dq += step
        pred += -dq if code & 8 else dq
        pred = max(-32768, min(32767, pred))
        idx = max(0, min(88, idx + IDX[code & 7]))
        out.append(code)
    return out

def pack(codes):
    if len(codes) & 1: codes = codes + [0]
    a = np.array(codes, dtype=np.uint8)
    return (a[0::2] | (a[1::2] << 4)).tobytes()

def norm(pcm, n):
    return np.pad(pcm, (0, max(0, n - len(pcm))))[:n]

# All music lives in the ARM9 binary (gen/music.bin): flashcart loaders can't be relied on for argv, FAT or card reads.
# Stage is what you hear nearly all the time. Thumbnail (pause + mode select) runs in lockstep at exactly half the rate.
A3 = os.path.join(PORT, "build/assets/3ds")
s0, s1 = ffmpeg_pcm(f"{A3}/intro.mp3", RATE), ffmpeg_pcm(f"{A3}/main.mp3", RATE)
n0, n1 = len(s0) & ~1, len(s1) & ~1                 # even: thumbnail sample boundaries stay on the 2:1 grid
s0, s1 = s0[:n0], s1[:n1]
t0, t1 = norm(ffmpeg_pcm(f"{A3}/options_intro.mp3", RATE // 2), n0 // 2), norm(ffmpeg_pcm(f"{A3}/options_select.mp3", RATE // 2), n1 // 2)
egg = ffmpeg_pcm(f"{A3}/easter_egg.mp3", EGG_RATE)
stage, thumb, eggd = pack(ima_codes(s0) + ima_codes(s1)), pack(ima_codes(t0) + ima_codes(t1)), pack(ima_codes(egg))
blob = stage + thumb + eggd
blob += bytes(-len(blob) & 3)
assert len(blob) <= MUSIC_BUDGET, len(blob)
words = np.frombuffer(blob, dtype="<u4")
crc = int(words.astype(np.uint64).sum() & 0xffffffff)
open(os.path.join(GEN, "music.bin"), "wb").write(blob)
print(f"music {len(blob)} bytes: stage {len(stage)} thumb {len(thumb)} egg {len(eggd)}")
with open(os.path.join(GEN, "music_gen.h"), "w") as o:
    o.write("/* Generated by tools/build_ds_assets.py - do not edit. */\n")
    o.write(f"#define MUSIC_RATE {RATE}\n#define EGG_RATE {EGG_RATE}\n#define IDX0 {IDX0}\n")
    o.write(f"#define INTRO_SAMPLES {n0}\n#define TOTAL_SAMPLES {n0 + n1}\n#define EGG_SAMPLES {len(egg)}\n")
    o.write(f"#define STAGE_BYTES {len(stage)}\n#define THUMB_BYTES {len(thumb)}\n")
    o.write(f"#define S0_PRED {int(s0[0])}\n#define S1_PRED {int(s1[0])}\n#define T0_PRED {int(t0[0])}\n#define T1_PRED {int(t1[0])}\n#define E_PRED {int(egg[0])}\n")
    o.write(f"#define MUSIC_WORDS {len(words)}\n#define MUSIC_SUM 0x{crc:08x}u\n")
os.makedirs(os.path.join(GEN, "sfx"), exist_ok=True)
for src_name, name, rate, fmt in (("pop.wav", "pop", 11025, "pcm_s16le"), ("pop_stage.wav", "popstage", 22050, "pcm_s16le"),
                                  ("crunch.wav", "crunch", 22050, "pcm_s16le"), ("hit.wav", "hit", 32000, "pcm_s16le"),
                                  ("lose.mp3", "lose", 16000, "pcm_u8")):
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", f"{A3}/{src_name}", "-ac", "1", "-ar", str(rate), "-c:a", fmt,
                    os.path.join(GEN, "sfx", name + ".wav")], check=True)
print("done")
