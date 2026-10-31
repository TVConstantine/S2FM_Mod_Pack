"""Tileable 3D noises for s2vfx_weather (examples/weather.vfx), baked into 2D atlases of z slices.

    python tests/weather_noise.py [content folder]    writes s2vfx_o_volume_shape.tga and s2vfx_o_volume_detail.tga
    python tests/weather_noise.py --check             checks the tiling and the atlas layout, writes nothing

Shape: 126^3 voxels, RGBA: R Perlin-Worley (the billows: Perlin fbm pushed down where Worley fbm is low), G, B and A
Worley at 6, 12 and 24 cells a side. Detail: 30^3, RGB: Worley at 4, 8 and 16 cells. Every noise repeats across the
volume (the lattice and the cells wrap), so the volume tiles in all three directions.

An atlas holds the slices z = 0, 1, ... as tiles left to right, top to bottom. A tile is n + 2 texels: the slice and a
one texel border copied from the opposite side, so a bilinear read inside a tile wraps without reaching the next one.
The atlases are 2048 x 1024 (16 x 8 tiles of 128) and 256 x 128 (8 x 4 tiles of 32): powers of two, as the texture
compiler wants them; the last tiles are unused.
"""
import os
import sys

import numpy as np
from PIL import Image

FOLDER = os.path.join(r"C:\Program Files (x86)\Steam\steamapps\common\Half-Life Alyx\content\core\materials\s2vfx_test")
SHAPE_N, SHAPE_COLS = 126, 16
DETAIL_N, DETAIL_COLS = 30, 8


def worley(n, cells, seed, offset=0):
    """Worley noise, n voxels a side, `cells` cells a side, one point in each: 1 on a point, 0 a cell's width away.
    offset shifts the voxels by whole volumes (for the tiling check)."""
    points = np.random.default_rng(seed).random((cells, cells, cells, 3))   # [cz, cy, cx] -> (x, y, z) in the cell
    coords = (np.arange(n) + offset * n + 0.5) * cells / n
    home = np.floor(coords).astype(int)
    out = np.empty((n, n, n), np.float32)
    for z in range(n):
        best = np.full((n, n), 1e9)
        for dz in (-1, 0, 1):
            cz = home[z] + dz
            for dy in (-1, 0, 1):
                cy = home[:, None] + dy
                for dx in (-1, 0, 1):
                    cx = home[None, :] + dx
                    p = points[cz % cells, cy % cells, cx % cells]
                    d2 = ((coords[None, :] - (cx + p[..., 0])) ** 2 + (coords[:, None] - (cy + p[..., 1])) ** 2 +
                          (coords[z] - (cz + p[..., 2])) ** 2)
                    best = np.minimum(best, d2)
        out[z] = 1.0 - np.clip(np.sqrt(best), 0.0, 1.0)
    return out


def perlin(n, period, seed, offset=0):
    """Gradient noise, n voxels a side, `period` lattice cells a side (the lattice wraps): about -1..1."""
    g = np.random.default_rng(seed).normal(size=(period, period, period, 3))
    g /= np.linalg.norm(g, axis=-1, keepdims=True)
    coords = (np.arange(n) + offset * n + 0.5) * period / n
    i0 = np.floor(coords).astype(int)
    f = coords - i0
    fade = f * f * f * (f * (f * 6.0 - 15.0) + 10.0)
    out = np.empty((n, n, n), np.float32)
    for z in range(n):
        acc = np.zeros((n, n))
        for cz in (0, 1):
            wz = fade[z] if cz else 1.0 - fade[z]
            for cy in (0, 1):
                wy = (fade if cy else 1.0 - fade)[:, None]
                for cx in (0, 1):
                    wx = (fade if cx else 1.0 - fade)[None, :]
                    grad = g[(i0[z] + cz) % period, (i0[:, None] + cy) % period, (i0[None, :] + cx) % period]
                    dot = (grad[..., 0] * (f[None, :] - cx) + grad[..., 1] * (f[:, None] - cy) + grad[..., 2] * (f[z] - cz))
                    acc += wx * wy * wz * dot
        out[z] = acc
    return out


def remap(v, lo, hi, a, b):
    return a + (v - lo) * (b - a) / (hi - lo)


def shape_volume(n=SHAPE_N):
    p = (perlin(n, 4, 1) + 0.5 * perlin(n, 8, 2) + 0.25 * perlin(n, 16, 3)) / 1.75
    p = np.clip(p * 0.5 + 0.5, 0.0, 1.0)
    w = 0.625 * worley(n, 4, 11) + 0.25 * worley(n, 8, 12) + 0.125 * worley(n, 16, 13)
    pw = np.clip(remap(p, w - 1.0, 1.0, 0.0, 1.0), 0.0, 1.0)
    # Perlin-Worley comes out squeezed into about 0.55..0.77: stretched over 0..1, so a coverage threshold means what
    # it says.
    lo, hi = np.percentile(pw, [0.5, 99.5])
    pw = np.clip((pw - lo) / (hi - lo), 0.0, 1.0)
    return np.stack([pw, worley(n, 6, 21), worley(n, 12, 22), worley(n, 24, 23)], -1)


def detail_volume(n=DETAIL_N):
    return np.stack([worley(n, 4, 31), worley(n, 8, 32), worley(n, 16, 33)], -1)


def atlas(vol, cols):
    n, c = vol.shape[0], vol.shape[-1]
    tile = n + 2
    rows = -(-n // cols)
    while rows & (rows - 1):
        rows += 1
    img = np.zeros((rows * tile, cols * tile, c), np.float32)
    padded = np.pad(vol, ((0, 0), (1, 1), (1, 1), (0, 0)), mode="wrap")
    for k in range(n):
        r, col = divmod(k, cols)
        img[r * tile:(r + 1) * tile, col * tile:(col + 1) * tile] = padded[k]
    return img


def save(img, path):
    mode = "RGBA" if img.shape[-1] == 4 else "RGB"
    Image.fromarray((np.clip(img, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8), mode).save(path)


def write(folder=FOLDER):
    save(atlas(shape_volume(), SHAPE_COLS), os.path.join(folder, "s2vfx_o_volume_shape.tga"))
    save(atlas(detail_volume(), DETAIL_COLS), os.path.join(folder, "s2vfx_o_volume_detail.tga"))


def check():
    # The noises repeat: one volume further on is the same volume.
    assert np.allclose(worley(12, 3, 5), worley(12, 3, 5, offset=1), atol=1e-5)
    assert np.allclose(perlin(12, 3, 5), perlin(12, 3, 5, offset=1), atol=1e-5)
    # Across the seam the noise is as smooth as anywhere: the step from the last voxel to the first is no bigger than
    # the largest step inside.
    w = worley(24, 4, 6)
    inside = np.abs(np.diff(w, axis=2)).max()
    assert np.abs(w[:, :, 0] - w[:, :, -1]).max() <= inside + 1e-6
    # The atlas: tile k is slice k, its border the opposite side, sizes powers of two.
    vol = np.random.default_rng(0).random((30, 30, 30, 3)).astype(np.float32)
    a = atlas(vol, 8)
    assert a.shape == (128, 256, 3)
    for k in (0, 7, 8, 29):
        r, col = divmod(k, 8)
        t = a[r * 32:(r + 1) * 32, col * 32:(col + 1) * 32]
        assert np.array_equal(t[1:31, 1:31], vol[k])
        assert np.array_equal(t[0, 1:31], vol[k][-1]) and np.array_equal(t[31, 1:31], vol[k][0])
        assert np.array_equal(t[1:31, 0], vol[k][:, -1]) and np.array_equal(t[1:31, 31], vol[k][:, 0])
    assert atlas(np.zeros((126, 126, 126, 4), np.float32), 16).shape == (1024, 2048, 4)
    # A saved TGA reads back with row 0 at the top (what the shader's tile rows assume).
    probe = np.zeros((4, 4, 3), np.float32)
    probe[0, 0] = 1.0
    path = os.path.join(os.environ.get("TEMP", "."), "s2vfx_noise_probe.tga")
    save(probe, path)
    back = np.asarray(Image.open(path))
    os.remove(path)
    assert back[0, 0, 0] == 255 and back[3, 0, 0] == 0
    print("volume noise: ok")


if __name__ == "__main__":
    if "--check" in sys.argv:
        check()
    else:
        write(sys.argv[1] if len(sys.argv) > 1 else FOLDER)
        print("written into", sys.argv[1] if len(sys.argv) > 1 else FOLDER)
