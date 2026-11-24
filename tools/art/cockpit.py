#!/usr/bin/env python3
"""Mantis Air: the cockpits (src/art_cockpit.h), from the cockpits sheet.

Each cockpit is drawn at the screen's full 320 x 240 (the display lays it over the 160 x 120 world, so its lines
and instruments stay sharp). The sheet's views put the glare shield near the middle of the frame, which leaves
little sky; to see out, each view is split at its glare shield: the windscreen and its frame above are stretched
taller, the panel below kept at the sheet's scale (its bottom, the floor, cropped). The windows (the sheet's white)
are cut out. Then quantised to one shared palette (index 0 is the window), and stored a row at a time as runs.

The live instruments go into the panel's own bezels: their places (measured on the processed views) are in
INSTR below and exported with the art.

  cockpit.py <assets/source> <src/art_cockpit.h> [preview_dir]
"""
import os, sys, numpy as np
from PIL import Image
from scipy import ndimage

SHEET = 'cockpits_sheet.jpg'
SW, SH = 320, 240
# crop (sheet), split (crop row: the glare shield's top), bottom (crop row that meets the screen's bottom),
# at (screen row the split lands on); roof (crop row: the roof's bottom edge; the roof keeps the panel's scale, only
# the glass between is stretched)
VIEWS = [
    dict(name='cub',     crop=(10, 5, 350, 215),   roof=21, split=86, bottom=196, at=138),
    dict(name='beaver',  crop=(355, 228, 722, 440), roof=19, split=84, bottom=176, at=146),
    dict(name='jet',     crop=(355, 455, 722, 665), roof=33, split=92, bottom=180, at=142),
    dict(name='fighter', crop=(10, 675, 350, 890),  roof=0,  split=92, bottom=200, at=140),
]
NCOL = 63


def window_mask(a):
    """The sheet's white (sky through the glass): big near-white regions, grown over the JPEG's pale fringe."""
    mn = a.min(2)
    white = mn > 205
    lbl, n = ndimage.label(white)
    if n:
        sizes = ndimage.sum(white, lbl, range(1, n + 1))
        keep = np.isin(lbl, 1 + np.nonzero(sizes > 150)[0])
    else:
        keep = white
    for _ in range(2):                                  # the fringe: pale pixels next to the window
        grow = ndimage.binary_dilation(keep) & (mn > 120)
        keep = keep | grow
    return keep


def resample(rgb, alpha, w, h):
    """Premultiplied area resample (no smearing of the dark frame into the window), alpha cut at a half."""
    pm = np.dstack([rgb * alpha[..., None], alpha]).astype(np.float32)
    ch = [np.asarray(Image.fromarray(pm[..., i]).resize((w, h), Image.LANCZOS)) for i in range(4)]
    a = np.clip(ch[3], 0, 1)
    out = np.dstack([np.clip(c / np.maximum(a, 1e-3), 0, 255) for c in ch[:3]])
    return out, a > 0.5


def build(src, v):
    sheet = np.asarray(Image.open(os.path.join(src, SHEET)).convert('RGB')).astype(np.float32)
    x0, y0, x1, y1 = v['crop']
    a = sheet[y0:y1, x0:x1]
    win = window_mask(a)
    alpha = (~win).astype(np.float32)
    k = SW / (x1 - x0)                                    # the panel's scale
    roof_at = int(round(v['roof'] * k))
    parts = []
    if v['roof']: parts.append(resample(a[:v['roof']], alpha[:v['roof']], SW, roof_at))
    parts.append(resample(a[v['roof']:v['split']], alpha[v['roof']:v['split']], SW, v['at'] - roof_at))
    parts.append(resample(a[v['split']:v['bottom']], alpha[v['split']:v['bottom']], SW, SH - v['at']))
    rgb = np.vstack([p_[0] for p_ in parts]); m = np.vstack([p_[1] for p_ in parts])
    # the panel below the glare shield is solid (no sky shows through gaps the resample opened)
    return rgb, m


def kmeans(px, k, iters=30, seed=1):
    rng = np.random.default_rng(seed)
    c = px[rng.choice(len(px), k, replace=False)].astype(np.float64)
    for _ in range(iters):
        l = np.concatenate([((px[i:i + 20000, None, :] - c[None]) ** 2).sum(2).argmin(1) for i in range(0, len(px), 20000)])
        for i in range(k):
            if (l == i).any(): c[i] = px[l == i].mean(0)
    return c


def quantise(views):
    allpx = np.concatenate([rgb[m] for rgb, m in views])
    rng = np.random.default_rng(2)
    sample = allpx[rng.choice(len(allpx), min(60000, len(allpx)), replace=False)]
    pal = kmeans(sample, NCOL)
    # order dark to light (the game relights the panel by scaling, the order is only for reading)
    pal = pal[np.argsort(pal @ [0.3, 0.59, 0.11])]
    out = []
    for rgb, m in views:
        idx = np.zeros(m.shape, np.uint8)
        flat = rgb[m].astype(np.float64)
        l = np.concatenate([((flat[i:i + 20000, None, :] - pal[None]) ** 2).sum(2).argmin(1) for i in range(0, len(flat), 20000)])
        idx[m] = l + 1
        # speckle: a lone pixel unlike all four neighbours takes the commonest of them
        p = np.pad(idx, 1, mode='edge')
        nb = np.stack([p[:-2, 1:-1], p[2:, 1:-1], p[1:-1, :-2], p[1:-1, 2:]])
        lone = (nb != idx[None]).all(0)
        for y, x in zip(*np.nonzero(lone)):
            vals, cnt = np.unique(nb[:, y, x], return_counts=True)
            if cnt.max() >= 3: idx[y, x] = vals[cnt.argmax()]
        out.append(idx)
    return pal, out


def rle(idx):
    """Rows of runs: (count-1 in the low 6 bits... ) simple bytes: [len, colour] pairs, a row ends with len 0."""
    data = []; rows = []
    for row in idx:
        rows.append(len(data))
        x = 0
        while x < len(row):
            c = row[x]; n = 1
            while x + n < len(row) and row[x + n] == c and n < 255: n += 1
            data += [n, int(c)]; x += n
    return data, rows


def preview(pal, idx, path, world=None):
    rgbp = np.vstack([[0, 0, 0], pal]).astype(np.uint8)
    img = rgbp[idx]
    if world is not None:
        w = np.asarray(world.convert('RGB').resize((SW, SH), Image.NEAREST))
        img = np.where((idx == 0)[..., None], w, img)
    else:
        img = np.where((idx == 0)[..., None], np.array([120, 190, 235], np.uint8), img)
    Image.fromarray(img.astype(np.uint8)).save(path)


def main():
    src, out = sys.argv[1], sys.argv[2]
    pdir = sys.argv[3] if len(sys.argv) > 3 else None
    built = [build(src, v) for v in VIEWS]
    pal, idxs = quantise(built)
    if pdir:
        world = Image.open(os.path.join(pdir, 'world.png')) if os.path.exists(os.path.join(pdir, 'world.png')) else None
        for v, idx in zip(VIEWS, idxs):
            preview(pal, idx, os.path.join(pdir, f"cock_{v['name']}.png"), world)
            np.save(os.path.join(pdir, f"cock_{v['name']}.npy"), idx)
        np.save(os.path.join(pdir, 'cock_pal.npy'), pal)
    # emissive: the lit greens (instrument light, the screens' text, the Mantis badge) glow, unshaded at night
    hsv = []
    for c in pal:
        mx, mn = c.max(), c.min()
        hsv.append((mx, (mx - mn) / max(mx, 1)))
    emis = [1 if (c[1] > 110 and c[1] > c[0] * 1.35 and c[1] > c[2] * 1.2 and s_ > 0.35) else 0 for c, (m_, s_) in zip(pal, hsv)]
    L = ['// generated by tools/art/cockpit.py from assets/source/cockpits_sheet.jpg: do not edit',
         '// the cockpits at the screen\'s 320 x 240: a shared palette (index 0 is the window: transparent), each view a',
         '// row at a time as runs ([count, colour] pairs; a row is 320 pixels), and for each of the world\'s 160 columns',
         '// the first of its 120 rows the cockpit covers to the bottom (no need to draw the world there)',
         '#pragma once', '#include <stdint.h>', '',
         f'static const uint8_t COCK_PAL[{len(pal) + 1}][4] = {{{{0, 0, 0, 0}}, ' +
         ', '.join(f'{{{int(c[0])}, {int(c[1])}, {int(c[2])}, {e}}}' for c, e in zip(pal, emis)) + '};   // r, g, b, glows']
    names = []
    tot = 0
    for v, idx in zip(VIEWS, idxs):
        d, r = rle(idx); tot += len(d) + 2 * len(r)
        nm = v['name'].upper()
        L.append(f'static const uint8_t COCK_{nm}_RUNS[{len(d)}] = {{' + ', '.join(map(str, d)) + '};')
        L.append(f'static const uint32_t COCK_{nm}_ROW[240] = {{' + ', '.join(map(str, r)) + '};')
        # the world's rows the cockpit covers: per world column, below the lowest window pixel of its two
        win = idx == 0
        bot = []
        for fx in range(160):
            low = -1
            for sx in (2 * fx, 2 * fx + 1):
                rows = np.nonzero(win[:, sx])[0]
                if len(rows): low = max(low, rows.max())
            bot.append(min(120, (low + 2) // 2))
        L.append(f'static const uint8_t COCK_{nm}_BOTTOM[160] = {{' + ', '.join(map(str, bot)) + '};')
        names.append(nm)
    L.append('struct CockArt { const uint8_t* runs; const uint32_t* row; const uint8_t* bottom; };')
    L.append('static const CockArt COCK_ART[4] = {' + ', '.join(f'{{COCK_{n}_RUNS, COCK_{n}_ROW, COCK_{n}_BOTTOM}}' for n in names) + '};')
    open(out, 'w').write('\n'.join(L) + '\n')
    print('cockpit bytes', tot, 'glowing colours', sum(emis))


if __name__ == '__main__':
    main()
