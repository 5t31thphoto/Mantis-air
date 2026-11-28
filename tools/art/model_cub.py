#!/usr/bin/env python3
"""Mantis Air: the Cub, as a 3D model built to the reference sheet's measurements.

Every number below was measured on the cleaned views (cards.load_views: the sheet's cut-outs, trimmed, static prop
removed), in each view's own pixels. Each view is drawn at its own scale, so each gets its own metres-per-pixel:

    top    span 308 px  = 10.7 m (the game's span)  ->  kT = 0.03474 m/px; length 204 px -> 7.09 m
    side   length 311 px = 7.09 m                    ->  kS = 0.02280 m/px; ground = the wheel's bottom, y 131
    front  span 283 px  = 10.7 m                     ->  kF = 0.03781 m/px

Where the views disagree the rules are: the side view decides lengths and heights, the top view decides span, widths
and the wing's planform, the front (and rear) view decides dihedral and what's seen end-on (gear track). CONFLICTS
lists each one and what was taken.

Model frame: x forward, y up, z right; metres; origin at the centre of gravity. Sheet frame: d = metres aft of the
spinner's tip, h = metres above the main wheels' bottom, r = metres right of the centreline.

    model_cub.py <assets/source> <outdir>      (writes previews: turntable, the sheet's angles beside the sheet)
"""
import os, sys, math, json, numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(__file__))
import cards

SPAN = 10.7
kT, kS, kF = SPAN / 308.0, 7.09 / 311.0, SPAN / 283.0
GROUND_SIDE_Y = 131                     # the side view's ground line (the main wheel's bottom)
TOP_CX, FRONT_CX = 153.5, 141.0         # centrelines in the top and front views

def sideD(x): return x * kS
def sideH(y): return (GROUND_SIDE_Y - y) * kS

# ---------------------------------------------------------------------------------------------- measurements
# Fuselage profile from the side view (x px: top y, bottom y), widths from the top view (y px: half-width px).
SIDE_PROFILE = [            # x, top, bottom (side view px)
    (15, 60, 74),           # spinner's base
    (17, 56, 84),           # cowl front
    (40, 55, 89),
    (60, 54, 92),           # windshield's foot
    (75, 38, 92),           # wing's leading edge: the cabin's roof is the wing's underside (y 38)
    (100, 38, 92),
    (150, 39, 91),
    (165, 42, 88),          # behind the wing
    (210, 44, 82),
    (245, 46, 76),          # the fin's foot
    (285, 52, 70),          # the tail post
]
TOP_WIDTH = [               # y (top view px), half-width px
    (12, 9), (16, 14), (30, 16.5), (45, 16.5), (100, 16.0), (105, 16.0), (130, 12.5), (150, 10.0), (160, 7.0),
    (180, 4.5), (196, 3.0)]
SPINNER = dict(tip_x=0, base_x=15, cy=67, r=7)            # side view px
WING = dict(le_y=53, te_y=99, tip_round=18,               # top view px: chord 46 px = 1.60 m, rounded tips 18 px
            bottom_y=38, top_y=21,                        # side view px: underside 2.12 m, top 2.51 m (at 30% chord)
            root_th_px=14, tip_th_px=8)                   # front view px: thickness at the root and the tip
STAB = dict(le_y=157, te_y=196, half_span=48.5, y_side=56, th=0.08)   # top view (planform), side (height)
FIN = [(245, 46), (280, 1), (295, 2), (308, 20), (311, 40), (305, 66), (285, 68)]   # side view px outline
FIN_TH = 0.14
MAIN_WHEEL = dict(x=76, cy=119, r=12, front_cx=(107, 176), width_px=12)   # side (position, radius), front (track)
GEAR_FOOT = dict(x0=68, x1=88, y=92, r=0.30)              # where the V legs leave the fuselage (side px), lateral m
STRUT_WING = dict(x0=85, x1=125, front_x=(60, 222))       # struts meet the wing (side px fore/aft; front px laterally)
STRUT_BODY = dict(x=105, y=85, r=0.52)                    # and the fuselage's lower side
TAIL_WHEEL = dict(x=297, y=77, r=4)                       # side px
HUB = dict(x=8, y=67)                                     # the prop's hub (side px)

CONFLICTS = [
    ("Fuselage width", "front view 43 px = 1.63 m", "top view 33 px = 1.15 m", "top (widths): 1.15 m"),
    ("Height of everything", "front view: wing top 3.40 m", "side view: wing top 2.51 m", "side (heights)"),
    ("Wing chord", "side view 80 px = 1.82 m", "top view 46 px = 1.60 m", "top (planform): 1.60 m"),
    ("Main gear, fore/aft", "top view pods at 1.43 m", "side view axle at 1.73 m", "side (lengths): 1.73 m"),
    ("Main gear, track", "top view pods at +-1.58 m", "front view wheels at +-1.30 m", "front (end-on): +-1.30 m"),
    ("Main wheel size", "front view radius 0.47 m, width 0.45 m", "side view radius 0.27 m", "side radius 0.27 m; width scaled with it: 0.26 m"),
    ("Dihedral", "-", "front view: wing level tip to root", "none"),
    ("Tail wheel", "side view: 1.09 m above the ground, drawn in level flight", "at the game's 11 deg three-point attitude it would hang 0.13 m clear", "leg lengthened so it touches"),
]

# ---------------------------------------------------------------------------------------------- geometry helpers
class Mesh:
    def __init__(s): s.v = []; s.t = []; s.obj = []; s.cur = 0   # t: (i0, i1, i2, part, colour or None); obj: its piece
    def vert(s, p): s.v.append(np.asarray(p, float)); return len(s.v) - 1
    def piece(s): s.cur += 1
    def tri(s, a, b, c, part, col=None): s.t.append((a, b, c, part, col)); s.obj.append(s.cur)
    def quad(s, a, b, c, d, part, col=None): s.tri(a, b, c, part, col); s.tri(a, c, d, part, col)

def interp(table, x):
    xs = [p[0] for p in table]
    if x <= xs[0]: return list(table[0][1:])
    if x >= xs[-1]: return list(table[-1][1:])
    for i in range(len(xs) - 1):
        if xs[i] <= x <= xs[i + 1]:
            t = (x - xs[i]) / (xs[i + 1] - xs[i])
            return [a + (b - a) * t for a, b in zip(table[i][1:], table[i + 1][1:])]

def build():
    m = Mesh()
    S = lambda d, h, r: (d, h, r)                     # sheet frame; converted to the model frame at the end

    # --- fuselage: octagons lofted through the measured stations
    def section(d, top, bot, hw, ch=0.3):
        c = ch * min(hw, (top - bot) * 0.5)
        pts = [(top, -hw + c), (top, hw - c), (top - c, hw), (bot + c, hw), (bot, hw - c), (bot, -hw + c), (bot + c, -hw), (top - c, -hw)]
        return [m.vert(S(d, h, r)) for h, r in pts]
    m.piece()
    rings = []
    for (x, ty, by) in SIDE_PROFILE:
        d = sideD(x)
        hw = interp(TOP_WIDTH, d / kT)[0] * kT
        rings.append(section(d, sideH(ty), sideH(by), hw, 0.45 if x < 60 else 0.3))
    for a, b in zip(rings, rings[1:]):
        for k in range(8):
            m.quad(a[k], a[(k + 1) % 8], b[(k + 1) % 8], b[k], 'body')
    tail = rings[-1]                                   # the tail post: closed
    for k in range(1, 7): m.tri(tail[0], tail[k + 1], tail[k], 'body')
    # --- spinner: a cone from the cowl's front to the tip
    sd0, sd1 = sideD(SPINNER['base_x']), sideD(SPINNER['tip_x']); sh = sideH(SPINNER['cy']); sr = SPINNER['r'] * kS
    m.piece()
    tip = m.vert(S(sd1, sh, 0.0))                        # a rounded spinner (a dome, as drawn), not a cone
    angs = np.linspace(0, 2 * math.pi, 9)[:-1]
    base = [m.vert(S(sd0, sh + sr * math.cos(a), sr * math.sin(a))) for a in angs]
    midd = sd0 + (sd1 - sd0) * 0.55
    mid = [m.vert(S(midd, sh + sr * 0.8 * math.cos(a), sr * 0.8 * math.sin(a))) for a in angs]
    for k in range(8):
        m.quad(base[k], base[(k + 1) % 8], mid[(k + 1) % 8], mid[k], 'spinner', 'spinner')
        m.tri(tip, mid[(k + 1) % 8], mid[k], 'spinner', 'spinner')
    m.cur = 1                                           # (the cowl's face: part of the fuselage's piece, so it faces out)
    front = rings[0]                                    # the cowl's face behind the spinner
    for k in range(1, 7): m.tri(front[0], front[k], front[k + 1], 'cowlface')

    # --- wing: one piece across the top of the cabin; rounded tips; thinner toward them; no dihedral
    le, te = WING['le_y'] * kT, WING['te_y'] * kT
    w_bot = sideH(WING['bottom_y']); w_top = sideH(WING['top_y'])
    th_root = w_top - w_bot
    th_tip = th_root * WING['tip_th_px'] / WING['root_th_px']
    half = SPAN / 2; rr = WING['tip_round'] * kT
    foil = [(0.0, 0.12), (0.04, 0.55), (0.15, 0.88), (0.30, 1.0), (0.60, 0.85), (1.0, 0.12), (1.0, 0.0), (0.0, 0.0)]
    zs = [-half, -half + rr * 0.35, -half + rr * 0.7, -half + rr, 0.0, half - rr, half - rr * 0.7, half - rr * 0.35, half]
    m.piece()
    secs = []
    for z in zs:
        out = max(0.0, abs(z) - (half - rr)) / rr      # 0 inside, 1 at the very tip
        cf = math.sqrt(max(0.0, 1 - out * out)) if out < 1 else 0.0
        mid = (le + te) / 2; c = (te - le) * max(cf, 0.06)
        # full thickness to where the tip's rounding starts (the side view sees only the tips then, purple as drawn);
        # the rounding thins toward the front view's tip thickness
        th = th_root if out == 0 else th_root + (th_tip * max(cf, 0.3) - th_root) * out
        secs.append([m.vert(S(mid - c / 2 + f * c, w_bot + (th_root - th) * 0.0 + u * th, z)) for f, u in foil])
    for a, b in zip(secs, secs[1:]):
        for k in range(len(foil)):
            m.quad(a[k], b[k], b[(k + 1) % len(foil)], a[(k + 1) % len(foil)], 'wing')
    for sec, flip in ((secs[0], True), (secs[-1], False)):          # the tips: closed (tiny)
        for k in range(1, len(foil) - 1):
            if flip: m.tri(sec[0], sec[k], sec[k + 1], 'wing')
            else: m.tri(sec[0], sec[k + 1], sec[k], 'wing')

    # --- tail: the fin (side view's outline, given thickness), the stabiliser (top view's ellipse)
    fo = [(sideD(x), sideH(y)) for x, y in FIN]
    m.piece()
    L = [m.vert(S(d, h, -FIN_TH / 2)) for d, h in fo]; R = [m.vert(S(d, h, FIN_TH / 2)) for d, h in fo]
    n = len(fo)
    for k in range(1, n - 1): m.tri(L[0], L[k + 1], L[k], 'fin'); m.tri(R[0], R[k], R[k + 1], 'fin')
    for k in range(n): m.quad(L[k], L[(k + 1) % n], R[(k + 1) % n], R[k], 'fin')
    s_h = sideH(STAB['y_side']); smid = (STAB['le_y'] + STAB['te_y']) / 2 * kT; sc0 = (STAB['te_y'] - STAB['le_y']) * kT
    hs = STAB['half_span'] * kT; th = STAB['th']
    zz = [-hs, -hs * 0.92, -hs * 0.7, -hs * 0.4, 0, hs * 0.4, hs * 0.7, hs * 0.92, hs]
    m.piece()
    st = []
    for z in zz:
        c = sc0 * math.sqrt(max(0.0, 1 - (z / hs) ** 2)); c = max(c, 0.05)
        st.append([m.vert(S(smid - c / 2, s_h + th / 2, z)), m.vert(S(smid + c / 2, s_h + th / 2, z)),
                   m.vert(S(smid + c / 2, s_h - th / 2, z)), m.vert(S(smid - c / 2, s_h - th / 2, z))])
    for a, b in zip(st, st[1:]):
        for k in range(4): m.quad(a[k], b[k], b[(k + 1) % 4], a[(k + 1) % 4], 'stab')

    # --- struts (V each side, wing to the fuselage's lower side) and the gear (V legs, big tundra wheels)
    def rod(p, q, w, part, col):
        m.piece()
        p = np.asarray(p, float); q = np.asarray(q, float); ax = q - p; ax /= np.linalg.norm(ax)
        up = np.array([0, 1.0, 0]) if abs(ax[1]) < 0.9 else np.array([1.0, 0, 0])
        u = np.cross(ax, up); u /= np.linalg.norm(u); v = np.cross(ax, u)
        cs = [(-1, -1), (1, -1), (1, 1), (-1, 1)]
        A = [m.vert(p + (u * a + v * b) * w / 2) for a, b in cs]; B = [m.vert(q + (u * a + v * b) * w / 2) for a, b in cs]
        for k in range(4): m.quad(A[k], A[(k + 1) % 4], B[(k + 1) % 4], B[k], part, col)
    for sgn in (-1, 1):
        wz = sgn * abs(STRUT_WING['front_x'][0] - FRONT_CX) * kF
        foot = S(sideD(STRUT_BODY['x']), sideH(STRUT_BODY['y']), sgn * STRUT_BODY['r'])
        rod(foot, S(sideD(STRUT_WING['x0']), w_bot, wz), 0.07, 'strut', 'strut')
        rod(foot, S(sideD(STRUT_WING['x1']), w_bot, wz), 0.07, 'strut', 'strut')
        axle = S(sideD(MAIN_WHEEL['x']), sideH(MAIN_WHEEL['cy']), sgn * abs(MAIN_WHEEL['front_cx'][0] - FRONT_CX) * kF)
        for gx in (GEAR_FOOT['x0'], GEAR_FOOT['x1']):
            rod(S(sideD(gx), sideH(GEAR_FOOT['y']), sgn * GEAR_FOOT['r']), axle, 0.09, 'gear', 'gear')
        # the wheel: an 8-sided drum; its outer face shows the side view's hub
        wr = MAIN_WHEEL['r'] * kS; ww = wr * (MAIN_WHEEL['width_px'] / 2) / (25 / 2) * 2
        ad, ah, az = axle
        outer = az + sgn * ww / 2; inner = az - sgn * ww / 2
        m.piece()
        ang = np.linspace(0, 2 * math.pi, 9)[:-1]
        Ro = [m.vert(S(ad + wr * math.cos(a), ah + wr * math.sin(a), outer)) for a in ang]
        Ri = [m.vert(S(ad + wr * math.cos(a), ah + wr * math.sin(a), inner)) for a in ang]
        for k in range(8): m.quad(Ro[k], Ro[(k + 1) % 8], Ri[(k + 1) % 8], Ri[k], 'tyre', 'tyre')
        for k in range(1, 7):
            if sgn > 0: m.tri(Ro[0], Ro[k], Ro[k + 1], 'wheel'); m.tri(Ri[0], Ri[k + 1], Ri[k], 'wheel')
            else: m.tri(Ro[0], Ro[k + 1], Ro[k], 'wheel'); m.tri(Ri[0], Ri[k], Ri[k + 1], 'wheel')
    # the tail wheel: its leg lengthened so it touches at the three-point attitude (see CONFLICTS)
    tw_d = sideD(TAIL_WHEEL['x']); tw_r = TAIL_WHEEL['r'] * kS * 1.6
    m.TAILWHEEL = (tw_d, tw_r)

    # --- to the model frame
    return m

def orient(m):
    """Every face turned outward: away from the middle of its own piece (each piece is convex or nearly)."""
    V = np.array(m.v)
    cent = {}
    for (i0, i1, i2, part, col), o in zip(m.t, m.obj):
        cent.setdefault((o, part), []).extend([i0, i1, i2])         # by piece and part: the spinner and the wing were one
    cent = {o: V[list(set(ix))].mean(0) for o, ix in cent.items()}
    out = []
    for (i0, i1, i2, part, col), o in zip(m.t, m.obj):
        n = np.cross(V[i1] - V[i0], V[i2] - V[i0])
        if np.dot(n, (V[i0] + V[i1] + V[i2]) / 3 - cent[(o, part)]) < 0: i1, i2 = i2, i1
        out.append((i0, i1, i2, part, col))
    m.t = out

def finish(m, ground_pitch=0.19, gear_h=1.25):
    """Sheet frame -> model frame, with the origin where the game's physics puts the centre of gravity: gear_h above
    the main wheels' contact patch when sitting at its three-point attitude. The tail wheel is then placed to touch."""
    V = np.array(m.v)
    d_cg = (WING['le_y'] + 0.25 * (WING['te_y'] - WING['le_y'])) * kT          # a quarter back along the wing
    c, s = math.cos(ground_pitch), math.sin(ground_pitch)
    # the main wheel's contact point (sheet frame) and its height in the pitched frame decide the origin's height
    wd, wh = sideD(MAIN_WHEEL['x']), 0.0
    fwd = d_cg - wd
    # pitched (nose up) by p: up' = up*c + fwd*s ; want contact up' = -gear_h  ->  up = (-gear_h - fwd*s)/c
    up_c = (-gear_h - fwd * s) / c
    h_cg = wh - up_c
    P = np.stack([d_cg - V[:, 0], V[:, 1] - h_cg, V[:, 2]], 1)
    # the tail wheel: on the ground line of the pitched frame, below the tail post
    tw_d, tw_r = m.TAILWHEEL
    tf = d_cg - tw_d
    t_up = (-gear_h - tf * s) / c + tw_r                                        # its centre, model frame
    m.v = list(P)
    post = np.array([tf, sideH(70) - h_cg, 0.0])
    cen = np.array([tf - 0.05, t_up, 0.0])
    def rod(p, q, w, part, col):
        m.piece()
        ax = q - p; ax /= np.linalg.norm(ax); u = np.array([0, 0, 1.0]); v = np.cross(ax, u)
        cs = [(-1, -1), (1, -1), (1, 1), (-1, 1)]
        A = [m.vert(p + (u * a + v * b) * w / 2) for a, b in cs]; B = [m.vert(q + (u * a + v * b) * w / 2) for a, b in cs]
        for k in range(4): m.quad(A[k], A[(k + 1) % 4], B[(k + 1) % 4], B[k], part, col)
    rod(post, cen, 0.06, 'gear', 'gear')
    ang = np.linspace(0, 2 * math.pi, 7)[:-1]
    m.piece()
    for side in (-0.04, 0.04):
        ring = [m.vert(cen + np.array([tw_r * math.cos(a), tw_r * math.sin(a), side])) for a in ang]
        for k in range(1, 5): m.tri(ring[0], ring[k], ring[k + 1], 'tyre', 'tyre') if side > 0 else m.tri(ring[0], ring[k + 1], ring[k], 'tyre', 'tyre')
    m.hub = np.array([d_cg - sideD(HUB['x']), sideH(HUB['y']) - h_cg, 0.0])
    orient(m)
    m.d_cg, m.h_cg = d_cg, h_cg
    return m

# ---------------------------------------------------------------------------------------------- the views as textures
def textures(src):
    p = [q for q in cards.PLANES if q['name'] == 'cub'][0]
    v = cards.load_views(src, p)
    T = {}
    for k, (a, mk, org) in v.items():
        T[k] = (np.clip(a, 0, 255).astype(np.uint8), mk)
    return T

# (the sheets' light is evened out where they're loaded: cards.bake_light)
FLAT = {'spinner': (100, 30, 90), 'strut': (24, 40, 44), 'gear': (28, 44, 48), 'tyre': (26, 26, 30)}

def face_uv(m, i0, i1, i2, part, T):
    """Which view paints this face, and where its corners fall on that view."""
    P = np.array([m.v[i0], m.v[i1], m.v[i2]])
    n = np.cross(P[1] - P[0], P[2] - P[0]); ln = np.linalg.norm(n)
    n = n / ln if ln > 1e-12 else np.array([0, 1.0, 0])
    d = m.d_cg - P[:, 0]; h = P[:, 1] + m.h_cg; r = P[:, 2]          # back to the sheet frame
    ax = np.abs(n)
    if part == 'cowlface' or (ax[0] >= ax[1] and ax[0] >= ax[2]):
        if n[0] > 0 or part == 'cowlface':                            # facing forward: the front view
            if part in ('body', 'cowlface'):                          # the cowl, registered to the front view's cowl box
                hw = interp(TOP_WIDTH, sideD(17) / kT)[0] * kT
                x = FRONT_CX - r * (21.0 / hw); y = 85 - (h - sideH(84)) * (40.0 / (sideH(56) - sideH(84)))
            else:                                                     # the wing's leading edge etc: span to span
                x = FRONT_CX - r / kF; y = 37 - (h - sideH(WING['bottom_y'])) * (15.0 / (sideH(WING['top_y']) - sideH(WING['bottom_y'])))
            return 'front', np.stack([x, y], 1)
        if 'rear' in T:                                               # facing aft: the rear view
            rw = T['rear'][0].shape[1]; rcx = rw / 2.0
            x = rcx + r / SPAN * rw; y = 40 - (h - sideH(WING['bottom_y'])) * (12.0 / (sideH(WING['top_y']) - sideH(WING['bottom_y'])))
            return 'rear', np.stack([x, y], 1)
    if ax[1] >= ax[2]:                                                # facing up (or down: the same paint, in shade)
        return 'top', np.stack([TOP_CX + r / kT, d / kT], 1)
    return 'side', np.stack([d / kS, GROUND_SIDE_Y - h / kS], 1)    # facing either side: the side view

# ---------------------------------------------------------------------------------------------- a software renderer (previews)
# ---------------------------------------------------------------------------------------------- a software renderer (previews)
def render(m, T, W, H, cam, look, fov=None, ortho=None, bg=(232, 236, 240), light=(0.35, 0.8, 0.45), uvf=None):
    uvf = uvf or face_uv
    cam = np.asarray(cam, float); look = np.asarray(look, float)
    f = look - cam; f /= np.linalg.norm(f)
    up = np.array([0, 1.0, 0]); rgt = np.cross(f, up)
    if np.linalg.norm(rgt) < 1e-6: rgt = np.array([0, 0, 1.0])
    rgt /= np.linalg.norm(rgt); u = np.cross(rgt, f)
    img = np.zeros((H, W, 3), np.float32); img[:] = bg
    zb = np.full((H, W), 1e9, np.float32)
    L = np.asarray(light, float); L /= np.linalg.norm(L)
    V = np.array(m.v)
    rel = V - cam
    X = rel @ rgt; Y = rel @ u; Z = rel @ f
    if ortho:
        sx = W / 2 + X * ortho; sy = H / 2 - Y * ortho; iw = np.ones_like(Z)
    else:
        F = W / 2 / math.tan(math.radians(fov) / 2)
        iw = 1.0 / np.maximum(Z, 1e-3); sx = W / 2 + X * F * iw; sy = H / 2 - Y * F * iw
    for (i0, i1, i2, part, col) in m.t:
        if not ortho and min(Z[i0], Z[i1], Z[i2]) < 0.05: continue
        P = np.array([m.v[i0], m.v[i1], m.v[i2]])
        n = np.cross(P[1] - P[0], P[2] - P[0]); ln = np.linalg.norm(n)
        if ln < 1e-12: continue
        n /= ln
        view_d = (P[0] - cam) if not ortho else f
        if np.dot(n, view_d) > 0: continue                            # back face
        shade = 0.45 + 0.55 * max(0.0, float(np.dot(n, L)))
        if col: tex = None; base = np.array(FLAT.get(col, (40, 40, 44)) if isinstance(col, str) else col, np.float32)
        else:
            vk, uv = uvf(m, i0, i1, i2, part, T)
            tex, tm = T[vk]
        xs = np.array([sx[i0], sx[i1], sx[i2]]); ys = np.array([sy[i0], sy[i1], sy[i2]]); ws = np.array([iw[i0], iw[i1], iw[i2]])
        x0 = max(int(math.floor(xs.min())), 0); x1 = min(int(math.ceil(xs.max())), W - 1)
        y0 = max(int(math.floor(ys.min())), 0); y1 = min(int(math.ceil(ys.max())), H - 1)
        if x0 > x1 or y0 > y1: continue
        area = (xs[1] - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (ys[1] - ys[0])
        if abs(area) < 1e-9: continue
        gy, gx = np.mgrid[y0:y1 + 1, x0:x1 + 1] + 0.5
        w0 = ((xs[1] - gx) * (ys[2] - gy) - (xs[2] - gx) * (ys[1] - gy)) / area
        w1 = ((xs[2] - gx) * (ys[0] - gy) - (xs[0] - gx) * (ys[2] - gy)) / area
        w2 = 1 - w0 - w1
        inside = (w0 >= -1e-4) & (w1 >= -1e-4) & (w2 >= -1e-4)
        if not inside.any(): continue
        pw = w0 * ws[0] + w1 * ws[1] + w2 * ws[2]
        if ortho:
            z = w0 * Z[i0] + w1 * Z[i1] + w2 * Z[i2]
        else:
            z = 1.0 / pw
        yy, xx = np.nonzero(inside)
        zz = z[yy, xx]; Y_ = yy + y0; X_ = xx + x0
        ok = zz < zb[Y_, X_] - 1e-4
        if not ok.any(): continue
        Y_, X_, zz = Y_[ok], X_[ok], zz[ok]
        if tex is None:
            c = base[None, :] * shade
        else:
            a0, a1, a2 = w0[yy, xx][ok] * ws[0], w1[yy, xx][ok] * ws[1], w2[yy, xx][ok] * ws[2]
            s_ = a0 + a1 + a2
            tu = (a0 * uv[0, 0] + a1 * uv[1, 0] + a2 * uv[2, 0]) / s_
            tv = (a0 * uv[0, 1] + a1 * uv[1, 1] + a2 * uv[2, 1]) / s_
            th, tw = tex.shape[:2]
            ti = np.clip(tv.astype(int), 0, th - 1); tj = np.clip(tu.astype(int), 0, tw - 1)
            c = tex[ti, tj].astype(np.float32)
            # outside the view's drawing (a part the view hides): the nearest colour the view does show along the row
            miss = ~tm[ti, tj]
            if miss.any():
                c[miss] = np.array([34, 96, 100], np.float32)
            c = c * shade
        img[Y_, X_] = c; zb[Y_, X_] = zz
    return np.clip(img, 0, 255).astype(np.uint8)

def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    m = finish(build())
    T = textures(src)
    print('triangles', len(m.t), 'vertices', len(m.v))
    span = SPAN
    # the sheet's own angles, orthographic, beside the sheet
    V = np.array(m.v); c0 = (V.min(0) + V.max(0)) / 2                 # framed on the model's middle, as the sheet is
    views = {
        'top':   dict(cam=c0 + (0, 30, 0.0001), look=c0, light=(0, 1, 0)),
        'side':  dict(cam=c0 + (0, 0, -30), look=c0, light=(0.2, 0.3, -1)),
        'front': dict(cam=c0 + (30, 0, 0), look=c0, light=(1, 0.3, 0.2)),
    }
    rows = []
    for k, cfg in views.items():
        ref = T[k][0].copy(); ref[~T[k][1]] = (232, 236, 240)
        refI = Image.fromarray(ref); s = min(300 / refI.height, 480 / refI.width)
        refI = refI.resize((int(refI.width * s), int(refI.height * s)), Image.NEAREST)
        px_m = {'top': 1 / kT, 'side': 1 / kS, 'front': 1 / kF}[k] * s          # the sheet's own scale, as shown
        if k == 'top': r = np.rot90(render(m, T, 300, 480, cfg['cam'], cfg['look'], ortho=px_m, light=cfg['light']), 1)   # nose up, as on the sheet
        else: r = render(m, T, 480, 300, cfg['cam'], cfg['look'], ortho=px_m, light=cfg['light'])
        canvas = Image.new('RGB', (980, 320), (255, 255, 255))
        canvas.paste(refI, (10 + (480 - refI.width) // 2, 10 + (300 - refI.height) // 2))
        canvas.paste(Image.fromarray(np.ascontiguousarray(r)), (490, 10))
        rows.append(canvas)
    sheet = Image.new('RGB', (980, 320 * len(rows)), (255, 255, 255))
    for i, c in enumerate(rows): sheet.paste(c, (0, i * 320))
    sheet.save(os.path.join(out, 'cub_vs_sheet.png'))
    # a turntable, three-quarter from above
    frames = []
    for i in range(48):
        a = i / 48 * 2 * math.pi
        cam = (math.cos(a) * 16.5, 5.0, math.sin(a) * 16.5)
        frames.append(render(m, T, 480, 300, cam, (0, 0.2, 0), fov=40))
    for i, f in enumerate(frames): Image.fromarray(f).save(os.path.join(out, f'turn_{i:03d}.png'))
    json.dump({'conflicts': CONFLICTS, 'triangles': len(m.t)}, open(os.path.join(out, 'cub_model.json'), 'w'), indent=1)
    print('exported', export(m, T))

def export(m, T, path='src/art_mesh_cub.h'):
    """The model into the game: its faces keep the cards' textures (the same views), in card texels."""
    import mesh_export as X
    dims = X.card_dims({k: (T[k][0], T[k][1], None) for k in T})
    tris = []
    for (i0, i1, i2, part, col) in m.t:
        if col: tris.append((i0, i1, i2, None, None, X.livery_index(FLAT[col]))); continue
        vk, uv = face_uv(m, i0, i1, i2, part, T)
        vh, vw = T[vk][1].shape; cw, ch = dims[vk]
        tris.append((i0, i1, i2, vk, uv * np.array([cw / vw, ch / vh]), X.fill_index(T[vk][0], T[vk][1], uv, ('cub', vk))))
    V = np.array(m.v)
    sp = sorted({i for (a, b, c, part, col) in m.t if part == 'spinner' for i in (a, b, c)})
    P = V[sp]; hub = (P[:, 0].min() + 0.05, P[:, 1].mean(), P[:, 2].mean())
    return X.write(path, 'CUB', V, tris, SPAN, 1.25, [(hub[0], hub[1], hub[2], 0.1307 * SPAN)])

if __name__ == '__main__':
    main()
