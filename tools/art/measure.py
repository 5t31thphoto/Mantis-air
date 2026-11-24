#!/usr/bin/env python3
"""Measure the sheet's views: cleaned silhouettes and their profiles, drawn as blueprints with a ruler, so the
landmarks (nose, wing, stabiliser, fin, wingtips, nacelles, wheels) can be read off and checked against each other.
  measure.py <assets/source> <outdir>"""
import sys, os, numpy as np
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(__file__))
import cards

def ruled(rgb, mask, step=10):
    """The view at 3x with a ruler every `step` sheet pixels (labelled every 50)."""
    h, w = mask.shape
    img = np.where(mask[..., None], rgb, 255).astype(np.uint8)
    im = Image.fromarray(img).resize((w * 3, h * 3), Image.NEAREST)
    d = ImageDraw.Draw(im)
    for x in range(0, w, step):
        d.line([(x * 3, 0), (x * 3, h * 3)], fill=(255, 0, 0) if x % 50 == 0 else (255, 200, 200))
        if x % 50 == 0: d.text((x * 3 + 2, 2), str(x), fill=(200, 0, 0))
    for y in range(0, h, step):
        d.line([(0, y * 3), (w * 3, y * 3)], fill=(0, 0, 255) if y % 50 == 0 else (200, 200, 255))
        if y % 50 == 0: d.text((2, y * 3 + 2), str(y), fill=(0, 0, 200))
    return im

if __name__ == '__main__':
    src, out = sys.argv[1], sys.argv[2]
    for p in cards.PLANES:
        for vn, (r, m, _) in cards.load_views(src, p).items():
            ruled(r, m).save(f"{out}/{p['name']}_{vn}.png")
            print(p['name'], vn, 'size (w, h)', m.shape[1], m.shape[0])
