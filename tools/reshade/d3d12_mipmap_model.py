# CPU model of ReShade's D3D12 mipmap compute shader (mipmap_cs_5_0.hlsl) and the bilinear + clamp variant.
import numpy as np, sys
def morton(i):
    x = i & 0x55555555; y = (i >> 1) & 0x55555555
    for s, m in ((1, 0x33333333), (2, 0x0F0F0F0F), (4, 0x00FF00FF), (8, 0x0000FFFF)):
        x = (x ^ (x >> s)) & m; y = (y ^ (y >> s)) & m
    return x, y
def size(base, m): return (max(base[0] >> m, 1), max(base[1] >> m, 1))
def run(img, levels, linear):
    W, H = img.shape[1], img.shape[0]
    mips = [img.astype(np.float32)] + [np.zeros((size((W, H), m)[1], size((W, H), m)[0]), np.float32) for m in range(1, levels)]
    def load(m, x, y):  # UAV load: out of bounds = 0
        a = mips[m]; return a[y, x] if x < a.shape[1] and y < a.shape[0] else np.float32(0)
    def clampload(m, x, y):
        a = mips[m]; return a[min(y, a.shape[0] - 1), min(x, a.shape[1] - 1)]
    def store(m, x, y, v):
        a = mips[m]
        if x < a.shape[1] and y < a.shape[0]: a[y, x] = v
    red = lambda a, b, c, d: (a + b + c + d) * np.float32(0.25)
    def redc(a, b, c, d, loc, ps):
        if linear:
            if loc[0] * 2 + 1 >= ps[0]: b, d = a, c
            if loc[1] * 2 + 1 >= ps[1]: c, d = a, b
        return red(a, b, c, d)
    for lvl in range(0, levels - 1, 6):
        num = levels - lvl; sw, sh = mips[lvl].shape[1], mips[lvl].shape[0]
        for gy in range((sh - 1) // 64 + 1):
            for gx in range((sw - 1) // 64 + 1):
                inter = [None] * 256; base = {}
                for t in range(256):
                    mx, my = morton(t); bx, by = gx * 64 + mx * 4, gy * 64 + my * 4; base[t] = (bx, by)
                    v = []
                    for ox, oy in ((0, 0), (2, 0), (0, 2), (2, 2)):
                        x, y = bx + ox, by + oy
                        f = clampload if linear else load
                        v.append(red(f(lvl, x, y), f(lvl, x + 1, y), f(lvl, x, y + 1), f(lvl, x + 1, y + 1)))
                    hx, hy = bx // 2, by // 2
                    for k, (ox, oy) in enumerate(((0, 0), (1, 0), (0, 1), (1, 1))): store(lvl + 1, hx + ox, hy + oy, v[k])
                    if num <= 2: continue
                    inter[t] = redc(*v, (hx // 2, hy // 2), size((sw, sh), 1)); store(lvl + 2, hx // 2, hy // 2, inter[t])
                for m in range(3, 7):
                    if num <= m: break
                    g = 1 << ((m - 2) * 2); new = list(inter)
                    for t in range(0, 256, g):
                        bx, by = base[t]; loc = (bx >> m, by >> m)
                        new[t] = redc(inter[t], inter[t + g // 4], inter[t + g // 2], inter[t + 3 * g // 4], loc, size((sw, sh), m - 1))
                        store(lvl + m, loc[0], loc[1], new[t])
                    inter = new
    return mips
def reference(img, levels):
    r = [img.astype(np.float64)]
    for m in range(1, levels):
        p = r[-1]; w, h = size((img.shape[1], img.shape[0]), m)
        a = np.zeros((h, w))
        for y in range(h):
            for x in range(w):
                xs = [min(2 * x + i, p.shape[1] - 1) for i in (0, 1)]; ys = [min(2 * y + j, p.shape[0] - 1) for j in (0, 1)]
                a[y, x] = sum(p[j, i] for j in ys for i in xs) / 4
        r.append(a)
    return r
rng = np.random.default_rng(1)
for W, H in [(256, 32), (64, 64), (128, 8), (100, 60), (32, 512), (300, 7)]:
    levels = int(np.floor(np.log2(max(W, H)))) + 1
    img = rng.random((H, W)).astype(np.float32); ref = reference(img, levels)
    for linear in (False, True):
        mips = run(img, levels, linear)
        bad = [m for m in range(1, levels) if np.abs(mips[m] - ref[m]).max() > 1e-5]
        print(f"{W}x{H} {'bilinear+clamp' if linear else 'original      '} wrong levels: {bad or 'none'}")
