#!/usr/bin/env python3
"""Mantis Air: the aircraft as sprite cards (src/art_cards.h).

Each aircraft is its sprite sheet views, kept as pixel art: top, side, front and (where drawn) rear. The game mounts
each on a flat card in the aircraft's own frame (top on the wing plane, side on the centre plane, front/rear across
the body) and draws the card the camera faces most squarely, projected with the aircraft's attitude, cross-fading
near 45 degrees. Nothing is reconstructed, so the art is never mangled; the views are only made to agree in size:

  * length and span from the top view; one height for side, front and rear (the generated views disagree on
    height: the geometric mean of what the side and front views say)
  * each view cut out of its sheet (background flood-filled away, strays erased), quantised to its family's
    15-colour palette (index 0 is transparent), and downscaled to its card size by a vote that keeps the
    one-pixel outlines; plus half and quarter sizes for distance

  cards.py <assets/source> <src/art_cards.h> [preview.png]
"""
import os, sys, numpy as np
from PIL import Image
from scipy import ndimage

SHEETS = {'mantis': 'aircraft_mantis_sheet.jpg', 'npc': 'aircraft_npc_bogey_sheet.jpg'}
# The props are drawn turning (the parts column's prop and blur-disc sprites), so the views' static props are taken
# out (measured on the ruled views, cut-out pixels):
#   unprop: (view, box, keep boxes) - erase what's in the box but not in a keep box (the nacelles, spinner, cowl)
#   undisc: (view, cx, cy, r, keep_r) - erase a blur disc's greys and blades, keeping its hub and the livery
#   Where a blade crossed the wing or cabin, the gap takes the neighbouring colour.
#   props: each prop's hub (top-view row, top-view column, end-view row, radius in end-view columns), its sprites
CUB_PROPS = dict(
    unprop=[('top', (112, 0, 194, 15), [(142, 0, 163, 16)]), ('side', (4, 18, 18, 112), [(0, 57, 18, 75)])],
    props=[(7, 152, 60, 37)], prop_sprites=((988, 26, 1082, 112), (1083, 22, 1180, 120)))
BEAVER_PROPS = dict(
    unprop=[('top', (68, 33, 140, 47), [(90, 0, 118, 214)]), ('top', (168, 33, 238, 47), [(190, 0, 218, 214)]),
            ('side', (54, 0, 69, 80), [])],
    undisc=[('front', 88, 45, 41, 9), ('front', 197, 45, 41, 9)],
    props=[(40, 103, 45, 38), (40, 203, 45, 38)], prop_sprites=((986, 240, 1082, 330), (1081, 236, 1180, 334)))
# Until its rear view is drawn, the Beaver's is built from its front view (props out, mirrored): the cockpit's face
# repainted in the tail's own colours (from the side view), a tail cone, and the parts column's tailplane across it.
BEAVER_REAR_STANDIN = dict(face=(141, 62, 25, 29), face_top_from=('side', 250, 52), face_bot_from=('side', 250, 76),
                           cone=(141, 60, 5), tailplane=((993, 341, 1064, 374), 93, 189, 41, 51))
# The afterburners: the parts column's flame (base at the nozzle) and its end-on glow (the bogey's ring; for the
# Fighter one is made from its flame's colours); nozzles on the rear view (column, row), the flame's length / length
# The Fighter's separate parts put on: its far fin (the shaded fin part) just behind the near one in the side view,
# and its two nozzles' ends in the rear view (where the afterburners come out)
FIGHTER_PARTS = dict(far_fin=((1040, 700, 1122, 764), 262, 0, 337, 56), nozzle_ends=[(106, 67, 6), (121, 67, 6)])
FIGHTER_FLAME = dict(flame_sprite=(1066, 789, 1173, 827), flame_flip=False, glow_sprite=None,
                     nozzles=[(106, 67), (121, 67)], flame_len=0.30)
BOGEY_FLAME = dict(flame_sprite=(911, 691, 1109, 769), flame_flip=True, glow_sprite=(1126, 704, 1179, 756),
                   nozzles=[(106, 66), (121, 66)], flame_len=0.34)
# emissive colours the flames are drawn in (they glow; the sun doesn't shade them): the palette's fixed entries
EMISSIVE = [('P_HUD_W', (242, 246, 250)), ('P_FIREFLY', (225, 255, 130)), ('P_HUD_G', (120, 255, 120)), ('P_HUD_A', (255, 190, 50)),
            ('P_AMBER', (255, 170, 40)), ('P_HUD_R', (255, 70, 60)), ('P_TAIL', (255, 40, 40)), ('P_TAIL_D', (120, 16, 24))]
# crops on the sheets (x0, y0, x1, y1). The sheets' columns: top, side, REAR, parts; but the Cubs' and the Beavers'
# third column is a FRONT view (their rears are separate files: cub_rear.jpg, beaver_rear.jpg). 'third' says which.
# top_rot: the top view drawn nose-right; erase: strays (cut-out pixels); gear: the retracting gear's boxes in each
# view's cut-out pixels (hidden when it's up)
PLANES = [
    dict(name='cub', sheet='mantis', top=(15, 3, 331, 214), side=(351, 40, 675, 186), front=(689, 48, 984, 174), third='front', rear='cub_rear.jpg',
         **CUB_PROPS),
    dict(name='beaver', sheet='mantis', top=(15, 214, 331, 438), side=(361, 258, 677, 417), front=(689, 264, 984, 418), third='front',
         rear='beaver_rear.jpg', rear_standin=BEAVER_REAR_STANDIN, **BEAVER_PROPS),
    dict(name='jet', sheet='mantis', top=(26, 437, 320, 672), side=(337, 484, 696, 624), front=(696, 492, 1002, 631),
         gear=dict(side=[(40, 104, 64, 129), (160, 104, 190, 129)], rear=[(136, 110, 158, 128), (88, 108, 112, 128), (182, 108, 206, 128)])),
    dict(name='fighter', sheet='mantis', top=(31, 673, 327, 889), side=(329, 726, 712, 856), front=(722, 727, 961, 854), top_rot=1,
         gear=dict(side=[(96, 90, 120, 117), (220, 90, 250, 117)], rear=[(66, 92, 90, 115), (102, 92, 124, 115), (136, 92, 160, 115)]),
         **FIGHTER_FLAME, parts=FIGHTER_PARTS),
    dict(name='npc_cub', sheet='npc', top=(15, 3, 331, 214), side=(351, 40, 675, 186), front=(689, 48, 984, 174), third='front', **CUB_PROPS),
    dict(name='npc_beaver', sheet='npc', top=(15, 214, 331, 438), side=(361, 258, 677, 417), front=(689, 264, 984, 418), third='front',
         rear_standin=BEAVER_REAR_STANDIN, **BEAVER_PROPS),
    dict(name='npc_jet', sheet='npc', top=(26, 437, 320, 672), side=(337, 484, 696, 624), front=(696, 492, 1002, 631),
         erase=[('rear', (278, 0, 310, 95))]),
    dict(name='npc_fighter', sheet='npc', top=(31, 673, 327, 889), side=(329, 726, 712, 856), front=(722, 727, 961, 854), top_rot=1,
         erase=[('rear', (172, 0, 240, 32))], **BOGEY_FLAME, parts=dict(nozzle_ends=[(106, 66, 6), (121, 66, 6)])),
]
CARD_MAX = 120          # the largest card's long side, texels (the plane is ~60-120 px wide on screen up close)

def cutout(img, box):
    """The sprite in the box: RGB and a mask. Background: the white the border reaches, and sizeable pure-white
    pockets (between struts, under a high wing)."""
    a = np.asarray(img.crop(box).convert('RGB')).astype(np.int32)
    whiteish = (a.min(2) > 200) & ((a.max(2) - a.min(2)) < 40)
    lab, n = ndimage.label(whiteish)
    border = set(np.unique(np.concatenate([lab[0], lab[-1], lab[:, 0], lab[:, -1]]))) - {0}
    if n:
        area = ndimage.sum(whiteish, lab, range(1, n + 1)); bright = ndimage.mean(a.min(2), lab, range(1, n + 1))
        border |= {i + 1 for i in range(n) if area[i] >= 40 and bright[i] >= 228}
    fg = ~np.isin(lab, list(border))
    lab2, n2 = ndimage.label(fg)
    if n2 > 1:
        sizes = ndimage.sum(fg, lab2, range(1, n2 + 1))
        fg = np.isin(lab2, [i + 1 for i, s in enumerate(sizes) if s >= max(30, sizes.max() * 0.015)])
    return a, fg

def trim(a, m):
    ys, xs = np.nonzero(m)
    return a[ys.min():ys.max() + 1, xs.min():xs.max() + 1], m[ys.min():ys.max() + 1, xs.min():xs.max() + 1], (xs.min(), ys.min())

def kmeans(px, k, iters=40, seed=1):
    rng = np.random.default_rng(seed)
    c = px[rng.choice(len(px), k, replace=False)].astype(np.float64)
    for _ in range(iters):
        l = ((px[:, None, :] - c[None]) ** 2).sum(2).argmin(1)
        for i in range(k):
            if (l == i).any(): c[i] = px[l == i].mean(0)
    return c

def nearest(rgb, pal):
    return ((rgb[..., None, :].astype(np.float64) - pal[None, None]) ** 2).sum(-1).argmin(-1)

def downscale(lbl, mask, w, h, dark, ss=5):
    """Labels onto w x h. Each texel samples ss x ss points of its footprint: transparent unless 45% are solid; the
    outline colour wins if 30% of the solid samples are it (keeps one-pixel outlines whole), else the commonest."""
    H, W = mask.shape
    o = (np.arange(ss) + 0.5) / ss
    ys = ((np.arange(h)[:, None] + o[None]) * H / h).astype(int).clip(0, H - 1)   # (h, ss)
    xs = ((np.arange(w)[:, None] + o[None]) * W / w).astype(int).clip(0, W - 1)   # (w, ss)
    out = np.zeros((h, w), np.uint8)
    for y in range(h):
        for x in range(w):
            m = mask[np.ix_(ys[y], xs[x])]
            if m.mean() < 0.45: continue
            v = lbl[np.ix_(ys[y], xs[x])][m]
            cnt = np.bincount(v, minlength=16)
            out[y, x] = dark if cnt[dark] >= 0.3 * cnt.sum() else cnt.argmax()
    return out

def load_views(src, p):
    sheet = Image.open(os.path.join(src, SHEETS[p['sheet']]))
    v = {}
    third = p.get('third', 'rear')                       # (the sheet's third column: rear, or the Cubs' front)
    for k, box in (('top', p['top']), ('side', p['side']), (third, p['front'])):
        a, m = cutout(sheet, box)
        if k == 'top' and p.get('top_rot'): a = np.rot90(a, 1); m = np.rot90(m, 1)
        v[k] = [a, m]
    if isinstance(p.get('rear'), str) and os.path.exists(os.path.join(src, p['rear'])):   # (a separate rear view, once drawn)
        r = Image.open(os.path.join(src, p['rear'])); a, m = cutout(r, (0, 0) + r.size); v['rear'] = [a, m]
    for (k, (x0, y0, x1, y1)) in p.get('erase', []): v[k][1][y0:y1, x0:x1] = False

    for k in list(v):
        a, m, org = trim(*v[k]); v[k] = [a, m, org]
    # (in the trimmed cut-out's pixels, as the ruled views measure them)
    # the static props out (see CUB_PROPS): erased, and the gaps they leave across the body filled from beside them
    gone = {k: np.zeros(v[k][1].shape, bool) for k in v}
    for (k, (x0, y0, x1, y1), keeps) in p.get('unprop', []):
        e = np.zeros_like(gone[k]); e[y0:y1, x0:x1] = True
        for (a0, b0, a1, b1) in keeps: e[b0:b1, a0:a1] = False
        gone[k] |= e & v[k][1]
    for (k, cx, cy, r, keep) in p.get('undisc', []):
        a_, m_, _ = v[k]
        yy, xx = np.mgrid[0:m_.shape[0], 0:m_.shape[1]]
        d2 = (xx - cx) ** 2 + (yy - cy) ** 2
        grey = ((a_.max(2) - a_.min(2)) < 30) & (a_.mean(2) > 40)          # blades and blur: greys, not the livery
        gone[k] |= m_ & grey & (d2 <= r * r) & (d2 > keep * keep)
    for k in v:
        if not gone[k].any(): continue
        a_, m_, org_ = v[k]
        keepm = m_ & ~gone[k]
        # a gap inside the body: body on both sides of it along its row or its column (within 32 px)
        def bridged(mk, axis):
            n = mk.shape[axis]; idx = np.arange(n)
            shp = [1, 1]; shp[axis] = n; idx = idx.reshape(shp)
            big = np.where(mk, idx, -10**6); last = np.maximum.accumulate(big, axis=axis)
            big2 = np.where(mk, idx, 10**6); nxt = np.flip(np.minimum.accumulate(np.flip(big2, axis), axis=axis), axis)
            return (idx - last <= 32) & (nxt - idx <= 32)
        hb = bridged(keepm, 1) & gone[k]; vb = bridged(keepm, 0) & gone[k] & ~hb
        body = hb | vb
        # filled along the stroke's crossing: a vertical blade's gap from the same row (stripes run straight through),
        # a horizontal bar's from the same column
        a_ = a_.copy()
        H_, W_ = keepm.shape
        for y, x in zip(*np.nonzero(hb)):
            row = np.nonzero(keepm[y])[0]
            l = row[row < x]; r = row[row > x]
            src_x = l[-1] if len(l) and (not len(r) or x - l[-1] <= r[0] - x) else r[0]
            a_[y, x] = a_[y, src_x]
        for y, x in zip(*np.nonzero(vb)):
            col = np.nonzero(keepm[:, x])[0]
            u = col[col < y]; d = col[col > y]
            src_y = u[-1] if len(u) and (not len(d) or y - u[-1] <= d[0] - y) else d[0]
            a_[y, x] = a_[src_y, x]
        v[k] = [a_, keepm | body, org_]
    pp = p.get('parts') or {}
    if pp.get('far_fin') and 'side' in v:                # (behind: only where the side view is empty)
        box, x0, y0, x1, y1 = pp['far_fin']
        sheet = Image.open(os.path.join(src, SHEETS[p['sheet']]))
        fa, fmk = cutout(sheet, box); fa, fmk, _ = trim(fa, fmk)
        w_, h_ = x1 - x0, y1 - y0
        fi = np.asarray(Image.fromarray(fa.astype(np.uint8)).resize((w_, h_), Image.NEAREST)).astype(np.int32)
        fm = np.asarray(Image.fromarray((fmk * 255).astype(np.uint8)).resize((w_, h_), Image.NEAREST)) > 127
        a_, m_, o_ = v['side']; a_ = a_.copy(); m_ = m_.copy()
        reg = a_[y0:y1, x0:x1]; rm = m_[y0:y1, x0:x1]; put = fm & ~rm
        reg[put] = fi[put]; rm[put] = True
        v['side'] = [a_, m_, o_]
    if pp.get('nozzle_ends') and 'rear' in v:            # dark rim, grey liner, black throat
        a_, m_, o_ = v['rear']; a_ = a_.copy(); m_ = m_.copy()
        yy, xx = np.mgrid[0:m_.shape[0], 0:m_.shape[1]]
        for (nx, ny, r) in pp['nozzle_ends']:
            d = np.hypot(xx - nx, yy - ny)
            a_[d <= r] = (36, 16, 40); a_[d <= r - 1.5] = (84, 86, 96); a_[d <= r - 3] = (16, 14, 20); m_[d <= r] = True
        v['rear'] = [a_, m_, o_]
    if 'rear' not in v and p.get('rear_standin') and 'front' in v:
        v['rear'] = list(rear_standin(src, p, v))
    for k in v: v[k][0] = bake_light(v[k][0], v[k][1], k)
    return {k: tuple(x) for k, x in v.items()}

# The sheets are drawn lit from above, with deep shading underneath; in the game the renderer's sun shades the aircraft
# again on top of that. So the textures are evened out here, once, in the sheets themselves (the cards and the models
# both take them from here; the renderer is unchanged):
#   a lift: a gamma on every colour, the dark end most;
#   a big soft cyan light from below: on the side, front and rear views, strongest at the bottom of each column of the
#   drawing and fading to nothing at its top (the top view looks down on the upper surfaces: the lift only).
BAKE_LIFT = 0.80                       # gamma; 1 = as drawn
BAKE_FILL = 0.55                       # the fill at the very bottom: +55% there
BAKE_TINT = np.array([0.55, 0.95, 1.0])
def bake_light(a, m, view):
    a = 255.0 * (np.clip(a, 0, 255) / 255.0) ** BAKE_LIFT
    if view == 'top' or not m.any(): return a
    H_, W_ = m.shape
    yy = np.arange(H_)[:, None].astype(float)
    has = m.any(0)
    top = np.where(has, m.argmax(0), 0).astype(float)
    bot = np.where(has, H_ - 1 - m[::-1].argmax(0), 1).astype(float)
    w = np.clip((yy - top[None, :]) / np.maximum(bot - top, 1.0)[None, :], 0, 1)
    k = np.ones(9) / 9                                               # soft across the columns too (no streaks)
    w = np.apply_along_axis(lambda r: np.convolve(np.pad(r, 4, mode='edge'), k, 'valid'), 1, w)
    return np.clip(a * (1.0 + BAKE_FILL * w[..., None] * BAKE_TINT[None, None, :]), 0, 255)

def rear_standin(src, p, v):
    """A rear view made from the front view, until one is drawn (see BEAVER_REAR_STANDIN)."""
    rs = p['rear_standin']
    a, m, org = v['front']
    a = a[:, ::-1].copy(); m = m[:, ::-1].copy()                  # (from behind, mirrored)
    H_, W_ = m.shape
    yy, xx = np.mgrid[0:H_, 0:W_]
    cx, cy, rx, ry = rs['face']
    inside = ((xx - cx) / rx) ** 2 + ((yy - cy) / ry) ** 2 <= 1.0
    rim = inside & (((xx - cx) / (rx - 2)) ** 2 + ((yy - cy) / (ry - 2)) ** 2 > 1.0)
    sv = v['side'][0]
    top_c = sv[rs['face_top_from'][2], rs['face_top_from'][1]]; bot_c = sv[rs['face_bot_from'][2], rs['face_bot_from'][1]]
    face = inside & m
    a[face & (yy < cy)] = top_c; a[face & (yy >= cy)] = bot_c
    a[rim & m] = (14, 16, 24)                                      # its outline
    ccx, ccy, cr = rs['cone']
    cone = (xx - ccx) ** 2 + (yy - ccy) ** 2 <= cr * cr
    a[cone] = (30, 46, 50); a[cone & ((xx - ccx) ** 2 + (yy - ccy) ** 2 > (cr - 1) ** 2)] = (14, 16, 24)
    # the tailplane (parts column), stretched across its span at its height
    sheet = Image.open(os.path.join(src, SHEETS[p['sheet']]))
    box, x0, x1, y0, y1 = rs['tailplane']
    ta, tmk = cutout(sheet, box); ta, tmk, _ = trim(ta, tmk)
    tw, th = x1 - x0, y1 - y0
    ti = np.asarray(Image.fromarray(ta.astype(np.uint8)).resize((tw, th), Image.NEAREST)).astype(np.int32)
    tm = np.asarray(Image.fromarray((tmk * 255).astype(np.uint8)).resize((tw, th), Image.NEAREST)) > 127
    reg = a[y0:y1, x0:x1]; reg[tm] = ti[tm]; m[y0:y1, x0:x1] |= tm
    return a, m, org

def family_palettes(src, views=None):
    """Each family's palette: 15 colours by k-means over all its views (index 0: transparent). Deterministic, so
    the cards and the meshes share it."""
    if views is None: views = {p['name']: load_views(src, p) for p in PLANES}
    pals = {}
    for fam in SHEETS:
        px = np.concatenate([a[m] for p in PLANES if p['sheet'] == fam for (a, m, _) in views[p['name']].values()])
        px = px[np.random.default_rng(0).choice(len(px), min(60000, len(px)), replace=False)].astype(np.float64)
        c = kmeans(px, 15); c = c[np.argsort(c @ [0.3, 0.59, 0.11])]
        pals[fam] = np.vstack([[0, 0, 0], c])
    return pals

def main():
    src, out = sys.argv[1], sys.argv[2]
    preview = sys.argv[3] if len(sys.argv) > 3 else None
    views = {p['name']: load_views(src, p) for p in PLANES}
    pals = family_palettes(src, views)
    lines = ['// Generated by tools/art/cards.py from assets/source (the aircraft sprite sheets). Do not edit.',
             '#pragma once', '#include <stdint.h>', 'namespace mantis {',
             '// a card: w x h texels, 4 bits each (0 transparent, 1-15 the family palette), rows top first, two per byte',
             '// (high nibble first); mip 0 full, 1 half, 2 quarter, one after another in px.',
             'struct Card { uint8_t w[3], h[3]; uint16_t off[3]; const uint8_t* px; uint8_t nGear; uint8_t gear[3][4]; };',
             '// an aircraft: its cards (front or rear may be missing, w[0] == 0: the other is seen from its back) and its size',
             '// in spans: length (top card), height (side, front and rear cards)',
             '// props: hub (forward of the middle / length, right / span, up from the wheels / height), radius / span;',
             '// blade, disc: the parts column\'s prop and its blur (seen face on)',
             '// nozzles: the afterburner (right / span, up from the wheels / height, its flame\'s length / length); flame, glow:',
             '// its sprites, in the emissive colours (FLAME_LUT)',
             'struct PlaneArt { Card top, side, front, rear; float len, hgt; uint8_t nProp; float prop[2][4]; const Card* blade; const Card* disc;',
             '                  uint8_t nNozzle; float nozzle[2][3]; const Card* flame; const Card* glow; };']
    prev_rows = []
    for p in PLANES:
        v = views[p['name']]; pal = pals[p['sheet']]
        dark = 1                                          # (the darkest colour: the outlines)
        ta, tm, _ = v['top']; sa, sm, _ = v['side']; fa, fm, _ = v['front' if 'front' in v else 'rear']
        S = 1.0; L = tm.shape[0] / tm.shape[1]
        h_side = L * sm.shape[0] / sm.shape[1]; h_front = fm.shape[0] / fm.shape[1]   # (the end view: front or rear)
        Hn = float(np.sqrt(h_side * h_front))             # one height for every view (they disagree)
        dims = {'top': (S, L), 'side': (L, Hn), 'front': (S, Hn), 'rear': (S, Hn)}
        big = max(S, L)
        cards = {}
        for k in ('top', 'side', 'front', 'rear'):
            if k not in v: cards[k] = None; continue
            a, m, org = v[k]
            lbl = nearest(a, pal[1:]) + 1
            cw, ch = dims[k]
            w0 = max(4, int(round(CARD_MAX * cw / big))); h0 = max(4, int(round(CARD_MAX * ch / big)))
            mips = []
            for lv in range(3):
                w, h = max(2, w0 >> lv), max(2, h0 >> lv)
                mips.append(downscale(lbl, m, w, h, dark))
            gear = []
            for (x0, y0, x1, y1) in p.get('gear', {}).get(k, []):     # (cut-out pixels -> mip-0 texels)
                sx, sy = w0 / m.shape[1], h0 / m.shape[0]
                gear.append((int(x0 * sx), int(y0 * sy), int(np.ceil(x1 * sx)), int(np.ceil(y1 * sy))))
            cards[k] = (mips, gear)
        tag = p['name'].upper()
        def emit(k):
            if cards[k] is None: return '{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, nullptr, 0, {{0}}}'
            mips, gear = cards[k]
            data = []; offs = []
            for mp in mips:
                offs.append(len(data))
                hh, ww = mp.shape
                for y in range(hh):
                    row = list(mp[y]) + ([0] if ww % 2 else [])
                    data += [(row[i] << 4) | row[i + 1] for i in range(0, len(row), 2)]
            name = f'CARD_{tag}_{k.upper()}'
            lines.append(f'static const uint8_t {name}[{len(data)}] = {{')
            for i in range(0, len(data), 24): lines.append('    ' + ', '.join(str(b) for b in data[i:i + 24]) + ',')
            lines.append('};')
            g = ', '.join('{' + ', '.join(str(c) for c in r) + '}' for r in gear[:3]) or '{0}'
            return (f'{{{{{mips[0].shape[1]}, {mips[1].shape[1]}, {mips[2].shape[1]}}}, {{{mips[0].shape[0]}, {mips[1].shape[0]}, {mips[2].shape[0]}}}, '
                    f'{{{offs[0]}, {offs[1]}, {offs[2]}}}, {name}, {len(gear[:3])}, {{{g}}}}}')
        parts = [emit(k) for k in ('top', 'side', 'front', 'rear')]
        # its props: hub as fractions (forward of the middle / length, right / span, up from the wheels / height),
        # radius / span; and the prop and blur-disc sprites
        props = []
        end = 'front' if 'front' in v else 'rear'
        eh, ew = v[end][1].shape; th_, tw_ = v['top'][1].shape
        for (trow, tcol, erow, er) in p.get('props', []):
            props.append((0.5 - trow / th_, tcol / tw_ - 0.5, 1.0 - erow / eh, er / ew))
        pref = ['nullptr', 'nullptr']
        if p.get('prop_sprites'):
            sheet = Image.open(os.path.join(src, SHEETS[p['sheet']]))
            for i, box in enumerate(p['prop_sprites']):
                a_, m_ = cutout(sheet, box); a_, m_, _ = trim(a_, m_)
                lbl = nearest(a_, pal[1:]) + 1
                sz = 48
                mp = [downscale(lbl, m_, max(2, sz >> l), max(2, sz >> l), dark) for l in range(3)]
                nm = f'PROP_{tag}_{i}'
                data = []; offs = []
                for q_ in mp:
                    offs.append(len(data))
                    for y in range(q_.shape[0]):
                        row = list(q_[y]) + ([0] if q_.shape[1] % 2 else [])
                        data += [(row[i_] << 4) | row[i_ + 1] for i_ in range(0, len(row), 2)]
                lines.append(f'static const uint8_t {nm}[{len(data)}] = {{' + ', '.join(map(str, data)) + '};')
                lines.append(f'static const Card {nm}_C = {{{{{mp[0].shape[1]}, {mp[1].shape[1]}, {mp[2].shape[1]}}}, {{{mp[0].shape[0]}, {mp[1].shape[0]}, {mp[2].shape[0]}}}, {{{offs[0]}, {offs[1]}, {offs[2]}}}, {nm}, 0, {{{{0}}}}}};')
                pref[i] = f'&{nm}_C'
        # its afterburner: nozzles (right / span, up from the wheels / height) on the rear view; the flame and glow,
        # quantised to the emissive colours (indices into FLAME_LUT)
        flame = ['nullptr', 'nullptr']; nozz = []
        if p.get('flame_sprite'):
            sheet = Image.open(os.path.join(src, SHEETS[p['sheet']]))
            ep = np.array([[0, 0, 0]] + [c for _, c in EMISSIVE], np.float64)
            def emis(box, w, h, flip):
                a_, m_ = cutout(sheet, box); a_, m_, _ = trim(a_, m_)
                if flip: a_ = a_[:, ::-1]; m_ = m_[:, ::-1]
                lbl = nearest(a_, ep[1:]) + 1
                return [downscale(lbl, m_, max(2, w >> l), max(2, h >> l), 1) for l in range(3)], (a_, m_)
            fm_, (fa_, fmm_) = emis(p['flame_sprite'], 64, 24, p['flame_flip'])
            if p.get('glow_sprite'): gm_, _ = emis(p['glow_sprite'], 24, 24, False)
            else:                                     # a glow from the flame's own colours: brightest in the middle
                cols = (nearest(fa_[fmm_], ep[1:]) + 1).ravel()
                order = sorted(set(cols.tolist()), key=lambda c: -ep[c] @ [0.3, 0.59, 0.11])
                g = np.zeros((24, 24), np.uint8); yy, xx = np.mgrid[0:24, 0:24]; r = np.hypot(xx - 11.5, yy - 11.5) / 12
                for i_, c in enumerate(order[:4]): g[(r <= 1.0 - i_ * 0.25) & (g == 0) | (r <= 1.0 - i_ * 0.25)] = c
                g[r > 1.0] = 0
                gm_ = [g, g[::2, ::2].copy(), g[::4, ::4].copy()]
            for i_, mp in enumerate((fm_, gm_)):
                nm = f'FLAME_{tag}_{i_}'
                data = []; offs = []
                for q_ in mp:
                    offs.append(len(data))
                    for y in range(q_.shape[0]):
                        row = list(q_[y]) + ([0] if q_.shape[1] % 2 else [])
                        data += [(row[j_] << 4) | row[j_ + 1] for j_ in range(0, len(row), 2)]
                lines.append(f'static const uint8_t {nm}[{len(data)}] = {{' + ', '.join(map(str, data)) + '};')
                lines.append(f'static const Card {nm}_C = {{{{{mp[0].shape[1]}, {mp[1].shape[1]}, {mp[2].shape[1]}}}, {{{mp[0].shape[0]}, {mp[1].shape[0]}, {mp[2].shape[0]}}}, {{{offs[0]}, {offs[1]}, {offs[2]}}}, {nm}, 0, {{{{0}}}}}};')
                flame[i_] = f'&{nm}_C'
            rh, rw = v['rear'][1].shape
            nozz = [(x / rw - 0.5, 1.0 - y / rh, p['flame_len']) for (x, y) in p['nozzles']]
        nz = ', '.join('{' + ', '.join(f'{x:.4f}f' for x in q_) + '}' for q_ in nozz[:2]) or '{0, 0, 0}'
        pr = ', '.join('{' + ', '.join(f'{x:.4f}f' for x in q_) + '}' for q_ in props[:2]) or '{0, 0, 0, 0}'
        lines.append(f'static const PlaneArt PLANE_{tag} = {{{parts[0]}, {parts[1]}, {parts[2]}, {parts[3]}, {L:.4f}f, {Hn:.4f}f, {len(props[:2])}, {{{pr}}}, {pref[0]}, {pref[1]}, '
                     f'{len(nozz[:2])}, {{{nz}}}, {flame[0]}, {flame[1]}}};')
        print(f"{p['name']:12s} length {L:.3f} span, height {Hn:.3f} (side said {h_side:.3f}, end view {h_front:.3f})  cards",
              {k: (c[0][0].shape[1], c[0][0].shape[0]) for k, c in cards.items() if c})
        if preview:
            tiles = []
            for k in ('top', 'side', 'front', 'rear'):
                if cards[k] is None: continue
                mp = cards[k][0][0]
                img = np.where((mp > 0)[..., None], pal[mp], 235).astype(np.uint8)
                tiles.append(Image.fromarray(img).resize((mp.shape[1] * 3, mp.shape[0] * 3), Image.NEAREST))
            row = Image.new('RGB', (sum(t.size[0] for t in tiles) + 12 * len(tiles), max(t.size[1] for t in tiles) + 8), (210, 210, 210))
            x = 0
            for t in tiles: row.paste(t, (x, 4)); x += t.size[0] + 12
            prev_rows.append(row)
    lines.append('// the flames\' colours: fixed palette entries that glow (index 0 transparent)')
    lines.append('#define FLAME_LUT_INIT {0, ' + ', '.join(n for n, _ in EMISSIVE) + ', 0, 0, 0, 0, 0, 0, 0}')
    for fam, pal in pals.items():
        lines.append(f'static const uint8_t LIVERY16_{fam.upper()}[16][3] = {{' + ', '.join('{' + ', '.join(str(int(round(c))) for c in col) + '}' for col in pal) + '};')
    lines.append('}')
    open(out, 'w').write('\n'.join(lines) + '\n')
    print('wrote', out, os.path.getsize(out), 'bytes')
    if preview:
        Wd = max(r.size[0] for r in prev_rows); Ht = sum(r.size[1] for r in prev_rows)
        img = Image.new('RGB', (Wd, Ht), (210, 210, 210)); y = 0
        for r in prev_rows: img.paste(r, (0, y)); y += r.size[1]
        img.save(preview); print('preview', preview)

if __name__ == '__main__':
    main()
