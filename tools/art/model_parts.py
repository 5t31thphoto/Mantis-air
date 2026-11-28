#!/usr/bin/env python3
"""Mantis Air: the Beaver, the Jet and the Fighter as 3D models, built the way the Cub is (model_cub.py): from the
sheet's views, measured, each part shaped by the silhouettes, textured from the same views (so the same texels as the
cards), with the sheets' light evened out where they're loaded (cards.bake_light).

Here the measuring is done on the views' masks: each part names the region of each view it lives in, and its outline
is read from there station by station. The rules are the Cub's: the side view decides lengths and heights, the top
view decides span, widths and planforms, the end view (front, or rear) decides what's only seen end-on (dihedral,
how far out the engines and floats sit, fin cant).

Sheet frame: d metres aft of the nose, h metres above the ground (the wheels' or the floats' bottom), r metres right.
Model frame (the game's): x forward, y up, z right, metres, the origin gearH above the ground at the aircraft's
ground attitude (aircraft.cpp's gearH, groundPitch).

    model_parts.py <assets/source> <outdir> [beaver|jet|fighter ...]   (previews in outdir, headers into src/)
"""
import os, sys, math, json, numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(__file__))
import cards, model_cub as MC, mesh_export as X

class Mesh(MC.Mesh):
    """The Cub's mesh, plus for each face a point inside the solid next to it: the face is turned away from it. (The
    Cub's rule, away from the middle of the piece, goes wrong on long or swept parts.)"""
    def __init__(s): super().__init__(); s.ref = []
    def tri(s, a, b, c, part, col=None, ref=None): super().tri(a, b, c, part, col); s.ref.append(ref)
    def quad(s, a, b, c, d, part, col=None, ref=None): s.tri(a, b, c, part, col, ref); s.tri(a, c, d, part, col, ref)

def orient(m):
    V = np.array(m.v)
    cent = {}
    for (i0, i1, i2, part, col), o in zip(m.t, m.obj): cent.setdefault((o, part), []).extend([i0, i1, i2])
    cent = {k: V[list(set(ix))].mean(0) for k, ix in cent.items()}
    out = []
    for (i0, i1, i2, part, col), o, ref in zip(m.t, m.obj, m.ref):
        n = np.cross(V[i1] - V[i0], V[i2] - V[i0])
        c = cent[(o, part)] if ref is None else np.asarray(ref, float)
        if np.dot(n, (V[i0] + V[i1] + V[i2]) / 3 - c) < 0: i1, i2 = i2, i1
        out.append((i0, i1, i2, part, col))
    m.t = out
FOIL = [(0.0, 0.12), (0.04, 0.55), (0.15, 0.88), (0.30, 1.0), (0.60, 0.85), (1.0, 0.12), (1.0, 0.0), (0.0, 0.0)]
SOLID = {'gear': (28, 44, 48), 'tyre': (26, 26, 30), 'strut': (24, 40, 44)}

def pwl(pts, x):
    """A piecewise-linear function given as [(x, y)...] (a single number: constant)."""
    if not isinstance(pts, (list, tuple)): return float(pts)
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    return float(np.interp(x, xs, ys))

class Plane:
    """One aircraft's views, registered: each view's pixels <-> the sheet frame."""
    def __init__(s, name, src, span, gear_h, pitch, end_fit=None):
        s.name = name
        s.p = [q for q in cards.PLANES if q['name'] == name][0]
        v = cards.load_views(src, s.p)
        s.T = {k: (np.clip(a, 0, 255).astype(np.uint8), m) for k, (a, m, o) in v.items()}
        s.end = 'front' if 'front' in s.T else 'rear'
        s.span, s.gear_h, s.pitch = span, gear_h, pitch
        tm = s.mask('top'); sm = s.mask('side'); em = s.mask(s.end)
        cols = np.nonzero(tm.any(0))[0]; rows = np.nonzero(tm.any(1))[0]
        s.kT = span / (cols[-1] - cols[0] + 1); s.cxT = (cols[0] + cols[-1]) / 2.0
        s.noseT, s.tailT = rows[0], rows[-1] + 1
        s.length = (s.tailT - s.noseT) * s.kT
        sc = np.nonzero(sm.any(0))[0]; sr = np.nonzero(sm.any(1))[0]
        s.noseS, s.tailS, s.groundS, s.topS = sc[0], sc[-1] + 1, sr[-1] + 1, sr[0]
        s.kS = s.length / (s.tailS - s.noseS)
        ec = np.nonzero(em.any(0))[0]; er = np.nonzero(em.any(1))[0]
        s.kE = span / (ec[-1] - ec[0] + 1); s.cxE = (ec[0] + ec[-1]) / 2.0
        # the end view's heights: its ground row and its top row matched to the side view's (they're drawn at
        # different scales): a straight line through the two
        s.groundE, s.topE = er[-1] + 1, er[0]
        s.kEv = ((s.groundS - s.topS) * s.kS) / (s.groundE - s.topE)
        s.m = Mesh()

    def mask(s, k): return s.T[k][1]
    # sheet frame <-> view pixels
    def sd(s, x): return (x - s.noseS) * s.kS
    def sh(s, y): return (s.groundS - y) * s.kS
    def td(s, y): return (y - s.noseT) * s.kT
    def tr(s, x): return (x - s.cxT) * s.kT
    def er(s, x): return (s.cxE - x) * s.kE if s.end == 'front' else (x - s.cxE) * s.kE
    def eh(s, y): return (s.groundE - y) * s.kEv
    def ty(s, d): return s.noseT + d / s.kT
    def tx(s, r): return s.cxT + r / s.kT
    def sx(s, d): return s.noseS + d / s.kS
    def sy(s, h): return s.groundS - h / s.kS
    def ex(s, r): return s.cxE - r / s.kE if s.end == 'front' else s.cxE + r / s.kE
    def ey(s, h): return s.groundE - h / s.kEv

    # mask readings: the first and last set pixel along a column / row, inside a window
    def col(s, k, x, y0, y1):
        m = s.mask(k); x = int(round(np.clip(x, 0, m.shape[1] - 1)))
        y0 = max(0, int(y0)); y1 = min(m.shape[0], int(y1))
        idx = np.nonzero(m[y0:y1, x])[0]
        return (None, None) if not len(idx) else (y0 + idx[0], y0 + idx[-1] + 1)
    def row(s, k, y, x0, x1):
        m = s.mask(k); y = int(round(np.clip(y, 0, m.shape[0] - 1)))
        x0 = max(0, int(x0)); x1 = min(m.shape[1], int(x1))
        idx = np.nonzero(m[y, x0:x1])[0]
        return (None, None) if not len(idx) else (x0 + idx[0], x0 + idx[-1] + 1)

    # ------------------------------------------------------------------------------------------------- parts
    def loft(s, part, x0, x1, ceil, floor, top_cols, n=12, mirror=False, ch=0.42, hgt=None, width=None, cap=(True, True)):
        """A body along the flight line (fuselage, nacelle, float, engine): side-view columns x0..x1; its top and
        bottom read from the side view between the ceil and floor rows (functions of x, side px); its width from
        the top view's row at the same station between top_cols (a function of the station: (c0, c1)). For a part
        the side view doesn't show (behind another), hgt gives (top, bottom) metres as a function of d; width
        likewise (half width, centre r) metres. mirror: a pair, at +-r."""
        m = s.m
        xs = x0 + (x1 - x0) * (0.5 - 0.5 * np.cos(np.linspace(0, math.pi, n)))      # (denser at the ends)
        stations = []
        for x in xs:
            d = s.sd(x)
            if hgt: top, bot = hgt(d)
            else:
                a, b = s.col('side', x, pwl(ceil, x), pwl(floor, x))
                if a is None: continue
                top, bot = s.sh(a), s.sh(b)
            if width: hw, rc = width(d)
            else:
                c0, c1 = top_cols(d) if callable(top_cols) else top_cols
                a, b = s.row('top', s.ty(d), c0, c1)
                if a is None: continue
                hw = (b - a) * s.kT / 2; rc = s.tr((a + b) / 2)
            stations.append((d, top, bot, max(hw, 0.03), rc))
        for sgn in ((1, -1) if mirror else (1,)):
            m.piece(); rings = []; cens = []
            for (d, top, bot, hw, rc) in stations:
                c = ch * min(hw, (top - bot) * 0.5); r0 = rc * sgn
                pts = [(top, -hw + c), (top, hw - c), (top - c, hw), (bot + c, hw), (bot, hw - c), (bot, -hw + c), (bot + c, -hw), (top - c, -hw)]
                rings.append([m.vert((d, h, r0 + r)) for h, r in pts]); cens.append(np.array([d, (top + bot) / 2, r0]))
            for i, (a, b) in enumerate(zip(rings, rings[1:])):
                for k in range(8): m.quad(a[k], a[(k + 1) % 8], b[(k + 1) % 8], b[k], part, ref=(cens[i] + cens[i + 1]) / 2)
            if cap[0]:
                for k in range(1, 7): m.tri(rings[0][0], rings[0][k], rings[0][k + 1], part)
            if cap[1]:
                for k in range(1, 7): m.tri(rings[-1][0], rings[-1][k + 1], rings[-1][k], part)
        return stations

    def wing(s, part, c0, c1, r0, r1, h_root, th_root, th_tip, n=16, skip=(), dihedral=None, side_h=None):
        """A lifting surface (wing, tailplane) from the top view's planform: columns c0..c1, its leading and trailing
        edges read between rows r0 and r1 (column ranges in skip, where something else covers it, are interpolated
        across). Height: h_root metres at the root, rising by dihedral(|r|) metres (from the end view) outward;
        thickness th_root to th_tip."""
        m = s.m
        cs = np.linspace(c0, c1 - 1, n)
        meas = []
        for c in cs:
            if any(a <= c < b for a, b in skip): meas.append(None); continue
            a, b = s.col('top', c, r0, r1)
            meas.append(None if a is None else (a, b))
        good = [i for i, v in enumerate(meas) if v]
        cs = cs[good[0]:good[-1] + 1]; meas = meas[good[0]:good[-1] + 1]; n = len(cs)
        good = [i for i, v in enumerate(meas) if v]
        le = np.interp(range(n), good, [meas[i][0] for i in good]); te = np.interp(range(n), good, [meas[i][1] for i in good])
        half = max(abs(s.tr(cs[0])), abs(s.tr(cs[-1])))
        m.piece(); secs = []
        for c, a, b in zip(cs, le, te):
            r = s.tr(c); t = min(1.0, abs(r) / half)
            th = th_root + (th_tip - th_root) * t
            h = h_root + (dihedral(abs(r)) if dihedral else 0.0)
            d0, d1 = s.td(a), s.td(b)
            if d1 - d0 < 0.05: d1 = d0 + 0.05
            secs.append([m.vert((d0 + f * (d1 - d0), h - th * 0.35 + u * th, r)) for f, u in FOIL])
        V = lambda sec: np.array([m.v[i] for i in sec]).mean(0)
        for a, b in zip(secs, secs[1:]):
            ref = (V(a) + V(b)) / 2
            for k in range(len(FOIL)): m.quad(a[k], b[k], b[(k + 1) % len(FOIL)], a[(k + 1) % len(FOIL)], part, ref=ref)
        for sec, other in ((secs[0], secs[1]), (secs[-1], secs[-2])):
            for k in range(1, len(FOIL) - 1): m.tri(sec[0], sec[k], sec[k + 1], part, ref=V(other))

    def fin(s, part, x0, x1, ceil, root_y, th, n=10, r=0.0, cant=0.0, mirror=False):
        """A vertical surface from the side view's outline: columns x0..x1, its top read below ceil, its foot at
        root_y (side px, a function of x). Thickness th metres; at r metres out, canted outward by cant radians
        (the end view's), mirrored for a pair."""
        m = s.m
        xs = np.linspace(x0, x1 - 1, n)
        top, bot = [], []
        for x in xs:
            ry = pwl(root_y, x)
            a, b = s.col('side', x, pwl(ceil, x), ry + 1)
            if a is None or a >= ry - 0.5: a = ry - 0.5
            top.append((s.sd(x), s.sh(a))); bot.append((s.sd(x), s.sh(ry)))
        outline = top + bot[::-1]
        for sgn in ((1, -1) if mirror else (1,)):
            m.piece()
            h0 = min(h for _, h in bot)
            a_ = cant * sgn
            def place(d, h, z):
                u = (h - h0) / max(math.cos(cant), 0.3)              # (the side view shows its height foreshortened)
                return np.array((d, h0 + u * math.cos(a_) - z * math.sin(a_), r * sgn + u * math.sin(a_) + z * math.cos(a_)))
            L = [m.vert(place(d, h, -th / 2)) for d, h in outline]; R = [m.vert(place(d, h, th / 2)) for d, h in outline]
            k_ = len(outline)
            tops = np.array(top); bots = np.array(bot)
            def mid(d):
                return place(d, (np.interp(d, tops[:, 0], tops[:, 1]) + np.interp(d, bots[:, 0], bots[:, 1])) / 2, 0.0)
            cL = place(outline[0][0], outline[0][1], -th); cR = place(outline[0][0], outline[0][1], th)
            for k in range(1, k_ - 1):
                m.tri(L[0], L[k + 1], L[k], part, ref=np.array(m.v[L[k]]) + (np.array(m.v[R[k]]) - np.array(m.v[L[k]])) * 2)
                m.tri(R[0], R[k], R[k + 1], part, ref=np.array(m.v[R[k]]) + (np.array(m.v[L[k]]) - np.array(m.v[R[k]])) * 2)
            for k in range(k_):
                dm = (outline[k][0] + outline[(k + 1) % k_][0]) / 2
                m.quad(L[k], L[(k + 1) % k_], R[(k + 1) % k_], R[k], part, ref=mid(dm))

    def rod(s, p, q, w, part='strut'):
        m = s.m; m.piece()
        p = np.asarray(p, float); q = np.asarray(q, float); ax = q - p; ax /= np.linalg.norm(ax)
        up = np.array([0, 1.0, 0]) if abs(ax[1]) < 0.9 else np.array([1.0, 0, 0])
        u = np.cross(ax, up); u /= np.linalg.norm(u); v = np.cross(ax, u)
        cs = [(-1, -1), (1, -1), (1, 1), (-1, 1)]
        A = [m.vert(p + (u * a + v * b) * w / 2) for a, b in cs]; B = [m.vert(q + (u * a + v * b) * w / 2) for a, b in cs]
        for k in range(4): m.quad(A[k], A[(k + 1) % 4], B[(k + 1) % 4], B[k], part, part)

    def wheel(s, d, h, r, rad, width):
        m = s.m; m.piece()
        ang = np.linspace(0, 2 * math.pi, 9)[:-1]
        Ro = [m.vert((d + rad * math.cos(a), h + rad * math.sin(a), r + width / 2)) for a in ang]
        Ri = [m.vert((d + rad * math.cos(a), h + rad * math.sin(a), r - width / 2)) for a in ang]
        for k in range(8): m.quad(Ro[k], Ro[(k + 1) % 8], Ri[(k + 1) % 8], Ri[k], 'tyre', 'tyre')
        for k in range(1, 7): m.tri(Ro[0], Ro[k], Ro[k + 1], 'wheel'); m.tri(Ri[0], Ri[k + 1], Ri[k], 'wheel')

    # ------------------------------------------------------------------------------------------------- finishing
    def finish(s, d_cg):
        """To the model frame: the origin at d_cg along the flight line, gear_h above the lowest point when sitting
        at the ground attitude (the wheels' or floats' bottoms touch)."""
        m = s.m
        V = np.array(m.v)
        x = d_cg - V[:, 0]
        c, sn = math.cos(s.pitch), math.sin(s.pitch)
        h_cg = (np.min(V[:, 1] * c + x * sn) + s.gear_h * c) / c
        m.v = list(np.stack([x, V[:, 1] - h_cg, V[:, 2]], 1))
        m.d_cg, m.h_cg = d_cg, h_cg
        m.ref = [None if r is None else np.array([d_cg - r[0], r[1] - h_cg, r[2]]) for r in m.ref]
        orient(m)
        return m

    def face_uv(s, m, i0, i1, i2, part, T):
        """Which view paints this face (the one its normal faces most squarely), and its corners on that view."""
        P = np.array([m.v[i0], m.v[i1], m.v[i2]])
        n = np.cross(P[1] - P[0], P[2] - P[0]); ln = np.linalg.norm(n)
        n = n / ln if ln > 1e-12 else np.array([0, 1.0, 0])
        d = m.d_cg - P[:, 0]; h = P[:, 1] + m.h_cg; r = P[:, 2]
        ax = np.abs(n)
        if ax[0] >= ax[1] and ax[0] >= ax[2]:
            k = s.end
            if n[0] < 0 and 'rear' in T: k = 'rear'
            if n[0] > 0 and 'front' in T: k = 'front'
            if k == 'front': x = s.cxE_of('front') - r / s.kE
            else: x = s.cxE_of('rear') + r / s.kE
            return k, np.stack([x, s.groundE - h / s.kEv], 1)
        if ax[1] >= ax[2]:
            return 'top', np.stack([s.tx(r), s.ty(d)], 1)
        return 'side', np.stack([s.sx(d), s.sy(h)], 1)

    def cxE_of(s, k):
        m = s.mask(k); c = np.nonzero(m.any(0))[0]
        return (c[0] + c[-1]) / 2.0

# ===================================================================================================== the aircraft
def beaver(src):
    """The Mantis Beaver: a high-wing twin on floats. Engines in long nacelles on the wing (the top view's pods, which
    run from the props back to the tailplane), floats on struts below (side and front views)."""
    P = Plane('beaver', src, 14.6, 1.9, 0.04)
    # the fuselage: the side view's outline below the wing and above the floats' struts; widths from the top view's
    # middle (the pods kept out)
    ceil = [(0, 40), (60, 40), (66, 36), (140, 36), (150, 44), (200, 50), (230, 56), (303, 66)]
    floor = [(0, 95), (300, 95)]
    P.loft('body', P.noseS + 1, P.tailS - 18, ceil, floor, (P.cxT - 26, P.cxT + 26), n=14)
    # the wing: across the top view between rows 44 and 104 (the pods and the fuselage interpolated across)
    wa, wb = P.col('side', 100, 20, 60)                                     # its height: the side view at mid chord
    h_w = P.sh(wb) + 0.1
    P.wing('wing', 0, P.mask('top').shape[1], 40, 106, h_w, 0.42, 0.22, n=18,
           skip=[(P.cxT - 62, P.cxT - 33), (P.cxT - 26, P.cxT + 26), (P.cxT + 33, P.cxT + 62)])
    # the nacelles: under the wing, where the end view has the props (row 45, at its two discs); from their spinners
    # (the top view's pods ahead of the wing) back to the wing's trailing edge (the side view's engine block ends
    # there); round, as wide as the top view's pod ahead of the wing
    pa, pb = P.row('top', 26, P.cxT + 30, P.cxT + 80)
    n_hw = (pb - pa) * P.kT / 2; n_r = P.tr((pa + pb) / 2)
    n_h = P.eh(45)
    d0, d1 = P.td(8), P.td(104)
    def n_shape(d):
        t = (d - d0) / (d1 - d0)
        k = min(1.0, 0.35 + 0.65 * math.sqrt(max(t, 0.0) / 0.25)) if t < 0.25 else (1.0 if t < 0.8 else 1.0 - 0.45 * (t - 0.8) / 0.2)
        return k
    nac = P.loft('nacelle', P.sx(d0), P.sx(d1), None, None, None, n=10, mirror=True,
                 hgt=lambda d: (n_h + n_hw * n_shape(d), n_h - n_hw * n_shape(d)), width=lambda d: (n_hw * n_shape(d), n_r))
    # the floats: the side view's (its top at row 111, below the struts), out where the end view has them, as wide
    # as the top view's pods behind the wing
    fa, fb = P.row('top', 150, P.cxT + 30, P.cxT + 80)
    f_hw = (fb - fa) * P.kT / 2
    ea, eb = P.row(P.end, P.groundE - 20, P.cxE + 20, P.mask(P.end).shape[1])
    f_r = abs(P.er((ea + eb) / 2))
    P.loft('float', P.noseS + 3, 228, [(0, 111), (300, 111)], [(0, 146), (300, 146)], None,
           n=12, mirror=True, width=lambda d: (f_hw, f_r))
    # the struts: float to fuselage (front and aft), and the spreader bar between the floats
    f_top = P.sh(111)
    for sgn in (1, -1):
        for x in (62, 128):
            P.rod((P.sd(x), f_top, sgn * f_r), (P.sd(x + 6), P.sh(92), sgn * 0.5), 0.09)
    P.rod((P.sd(95), f_top - 0.1, f_r), (P.sd(95), f_top - 0.1, -f_r), 0.07)
    # the tail: the fin from the side view, the tailplane from the top view
    P.fin('fin', 225, P.tailS, 0, [(225, 60), (303, 72)], 0.16)
    st_a, st_b = P.col('side', 290, 60, 90)
    P.wing('stab', P.cxT - 58, P.cxT + 59, 176, 214, P.sh(st_b) - 0.05, 0.14, 0.08, n=10, skip=[(P.cxT - 6, P.cxT + 6)])
    d_cg = P.td(54 + 0.25 * 50)
    m = P.finish(d_cg)
    # props: at the nacelles' fronts, the cards' radius
    hubs = [(m.d_cg - P.td(40), n_h - m.h_cg, sgn * n_r, 0.1343 * P.span) for sgn in (1, -1)]   # (the props: top row 40)
    return P, m, hubs

def gear_from_boxes(P, wr, w):
    """Retracting gear where the cards' gear boxes are: each side box a leg (its column; the axle at the box's
    bottom less the wheel), each end-view box how far out a leg is (the middle one: the nose leg)."""
    sb = P.p['gear']['side']; eb = sorted(P.p['gear']['rear'], key=lambda b: b[0])
    rs = [P.er((b[0] + b[2]) / 2) for b in eb]
    nose_x = (sb[0][0] + sb[0][2]) / 2; main_x = (sb[1][0] + sb[1][2]) / 2; top_y = min(sb[0][1], sb[1][1])
    legs = [(nose_x, 0.0)] + [(main_x, r) for r in rs if abs(r) > 0.3]
    for (x, r_) in legs:
        P.rod((P.sd(x), P.sh(top_y), r_), (P.sd(x), wr, r_), 0.12, 'gear')
        P.wheel(P.sd(x), wr, r_, wr, w)

def jet(src):
    """The Mantis Jet: a low-wing twinjet airliner, swept wing with winglets, engines under the wing."""
    P = Plane('jet', src, 12.0, 1.4, 0.0)
    ceil = [(0, 40), (250, 48), (300, 60), (347, 62)]
    floor = [(0, 112), (100, 104), (347, 104)]
    P.loft('body', P.noseS + 1, P.tailS - 4, ceil, floor, (P.cxT - 20, P.cxT + 20), n=16)
    # the wing: low, swept; its height from the end view's root, dihedral from the end view out to the tip
    em = P.mask(P.end)
    def dih(r):
        x = P.ex(r); a, b = P.col(P.end, x, P.groundE - 60, P.groundE - 18)
        return 0.0 if a is None else 0.0
    wa, wb = P.col(P.end, P.ex(P.span * 0.3), 60, 105)
    h_mid = P.eh((wa + wb) / 2) if wa is not None else P.sh(95)
    wa2, wb2 = P.col(P.end, P.ex(P.span * 0.12), 60, 105)
    h_in = P.eh((wa2 + wb2) / 2) if wa2 is not None else h_mid
    slope = (h_mid - h_in) / (P.span * 0.18)
    P.wing('wing', 0, P.mask('top').shape[1], 55, 150, h_in - slope * P.span * 0.12, 0.30, 0.10, n=18,
           skip=[(P.cxT - 60, P.cxT - 32), (P.cxT - 17, P.cxT + 17), (P.cxT + 32, P.cxT + 60)],
           dihedral=lambda r: slope * r)
    # the engines: under the wing, the top view's pods ahead of it (lengthwise and width), the side view's nacelle
    # (heights), out where the end view's rings are
    ea, eb = P.row(P.end, P.groundE - 18, P.cxE + 30, em.shape[1] - 40)
    e_r = abs(P.er((ea + eb) / 2)) if ea is not None else 2.6
    na, nb = P.col('side', 130, 80, 125)
    e_top, e_bot = P.sh(na), P.sh(nb)
    e_hw = (e_top - e_bot) / 2 * 0.95
    P.loft('engine', 108, 165, None, None, None, n=8, mirror=True,
           hgt=lambda d: (e_top, e_bot), width=lambda d: (e_hw, e_r))
    # the tail
    P.fin('fin', 245, P.tailS, 0, [(245, 60), (347, 56)], 0.22)
    sa, sb = P.col(P.end, P.cxE + 40, 40, 80)
    h_st = P.eh((sa + sb) / 2) if sa is not None else P.sh(55)
    P.wing('stab', P.cxT - 55, P.cxT + 56, 168, 222, h_st, 0.16, 0.08, n=10, skip=[(P.cxT - 8, P.cxT + 8)])
    gear_from_boxes(P, 0.30, 0.22)
    d_cg = P.td(95)
    m = P.finish(d_cg)
    return P, m, []

def fighter(src):
    """The Mantis Fighter: a twin-tail delta-ish fighter."""
    P = Plane('fighter', src, 10.0, 1.6, 0.0)
    ceil = [(0, 30), (60, 40), (140, 44), (260, 44), (370, 50)]
    floor = [(0, 92), (370, 92)]
    P.loft('body', P.noseS + 1, P.tailS - 6, ceil, floor, (P.cxT - 34, P.cxT + 34), n=16, ch=0.35)
    # the canopy: the side view's bubble above the body's top, as wide as the top view's glass (about a fifth of the body)
    P.loft('canopy', 66, 150, [(0, 20), (370, 20)], [(0, 46), (370, 46)], None, n=8, ch=0.5,
           width=lambda d: (0.38, 0.0))
    em = P.mask(P.end)
    wa, wb = P.col(P.end, P.ex(P.span * 0.35), 40, 85)
    h_w = P.eh((wa + wb) / 2) if wa is not None else P.sh(70)
    P.wing('wing', 0, P.mask('top').shape[1], 95, 240, h_w, 0.26, 0.06, n=18, skip=[(P.cxT - 30, P.cxT + 30)])
    P.wing('stab', P.cxT - 75, P.cxT + 76, 236, 284, h_w - 0.05, 0.12, 0.05, n=12, skip=[(P.cxT - 30, P.cxT + 30)])
    # the fins: the side view's outline, out and canted as the end view's V
    fa, fb = P.row(P.end, 20, 0, P.cxE)
    ga, gb = P.row(P.end, 45, 0, P.cxE)
    if fa is not None and ga is not None:
        dx = ((ga + gb) / 2 - (fa + fb) / 2) * P.kE; dy = 25 * P.kEv
        cant = math.atan2(dx, dy); r_f = abs(P.er((ga + gb) / 2))
    else: cant, r_f = 0.45, 0.8
    P.fin('fin', 262, P.tailS - 10, 0, [(262, 52), (360, 52)], 0.12, r=r_f, cant=cant, mirror=True)
    gear_from_boxes(P, 0.28, 0.2)
    d_cg = P.td(170)
    m = P.finish(d_cg)
    return P, m, []

BUILD = {'beaver': beaver, 'jet': jet, 'fighter': fighter}

def preview(P, m, out):
    T = P.T
    V = np.array(m.v); c0 = (V.min(0) + V.max(0)) / 2
    rows = []
    for k, cam, rot in (('top', c0 + (0, 40, 0.0001), True), ('side', c0 + (0, 0, -40), False), (P.end, c0 + ((40 if P.end == 'front' else -40), 0, 0), False)):
        ref = T[k][0].copy(); ref[~T[k][1]] = (232, 236, 240)
        refI = Image.fromarray(ref); sc = min(300 / refI.height, 480 / refI.width)
        refI = refI.resize((int(refI.width * sc), int(refI.height * sc)), Image.NEAREST)
        px_m = {'top': 1 / P.kT, 'side': 1 / P.kS}.get(k, 1 / P.kE) * sc
        uvf = lambda mm, a, b, c, part, TT: P.face_uv(mm, a, b, c, part, TT)
        if rot: r = np.rot90(MC.render(m, T, 300, 480, cam, c0, ortho=px_m, light=(0, 1, 0), uvf=uvf), 1)
        else: r = MC.render(m, T, 480, 300, cam, c0, ortho=px_m, light=(0.2, 0.3, -1) if k == 'side' else (1, 0.3, 0.2), uvf=uvf)
        cv = Image.new('RGB', (980, 320), (255, 255, 255))
        cv.paste(refI, (10 + (480 - refI.width) // 2, 10 + (300 - refI.height) // 2)); cv.paste(Image.fromarray(np.ascontiguousarray(r)), (490, 10))
        rows.append(cv)
    sheet = Image.new('RGB', (980, 320 * len(rows)), (255, 255, 255))
    for i, cv in enumerate(rows): sheet.paste(cv, (0, i * 320))
    sheet.save(os.path.join(out, f'{P.name}_vs_sheet.png'))
    R = max(P.span, P.length) * 1.45
    shots = []
    for i, el in ((4, 0.3), (20, 0.3), (12, -0.15), (36, 0.5)):
        a = i / 48 * 2 * math.pi
        cam = (math.cos(a) * R, R * el, math.sin(a) * R)
        shots.append(MC.render(m, T, 480, 300, cam, (0, 0.2, 0), fov=40, uvf=lambda mm, a_, b_, c_, part, TT: P.face_uv(mm, a_, b_, c_, part, TT)))
    cv = Image.new('RGB', (960, 600))
    for k, f in enumerate(shots): cv.paste(Image.fromarray(f), ((k % 2) * 480, (k // 2) * 300))
    cv.save(os.path.join(out, f'{P.name}_angles.png'))

def export(P, m, hubs, retract, path):
    dims = X.card_dims({k: (P.T[k][0], P.T[k][1], None) for k in P.T})
    tris = []
    for (i0, i1, i2, part, col) in m.t:
        g = 0x80 if retract and part in ('gear', 'tyre', 'wheel') else 0
        if col: tris.append((i0, i1, i2, None, None, X.livery_index(SOLID.get(col, (30, 30, 34))) | 0, g)); continue
        vk, uv = P.face_uv(m, i0, i1, i2, part, P.T)
        vh, vw = P.T[vk][1].shape; cw, ch = dims[vk]
        fill = X.fill_index(P.T[vk][0], P.T[vk][1], uv, (P.name, vk))
        tris.append((i0, i1, i2, vk, uv * np.array([cw / vw, ch / vh]), fill, g))
    return X.write(path, P.name.upper(), np.array(m.v), tris, P.span, P.gear_h, hubs)

def main():
    src, out = sys.argv[1], sys.argv[2]
    names = sys.argv[3:] or list(BUILD)
    os.makedirs(out, exist_ok=True)
    root = os.path.join(os.path.dirname(__file__), '..', '..')
    for n in names:
        P, m, hubs = BUILD[n](src)
        preview(P, m, out)
        print(n, 'triangles', len(m.t), 'vertices', len(m.v),
              'exported', export(P, m, hubs, n in ('jet', 'fighter'), os.path.join(root, 'src', f'art_mesh_{n}.h')))

if __name__ == '__main__':
    main()
