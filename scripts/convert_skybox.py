#!/usr/bin/env python3
"""Convert an equirectangular HDR/EXR image to 6 cubemap face PNGs.

Usage:
    python scripts/convert_skybox.py <input.exr|input.hdr> <output_dir> [--size 512]

Output:
    output_dir/right.png, left.png, top.png, bottom.png, front.png, back.png

The face convention matches Vulkan cubemap layout:
    +X (right), -X (left), +Y (top), -Y (bottom), +Z (front), -Z (back)

Game coordinate system: X=right, Y=forward, Z=up.
"""

import sys
import os
import numpy as np
from PIL import Image

try:
    import OpenEXR
    import Imath
    HAS_OPENEXR = True
except ImportError:
    HAS_OPENEXR = False


def load_hdr(path: str) -> np.ndarray:
    """Load an equirectangular HDR/EXR image as float32 RGB array."""
    ext = os.path.splitext(path)[1].lower()

    if ext == ".exr":
        if not HAS_OPENEXR:
            print("ERROR: OpenEXR package required. Run: pip install OpenEXR")
            sys.exit(1)
        exr = OpenEXR.InputFile(path)
        header = exr.header()
        dw = header["dataWindow"]
        w = dw.max.x - dw.min.x + 1
        h = dw.max.y - dw.min.y + 1
        pt = Imath.PixelType(Imath.PixelType.FLOAT)
        channels = []
        for ch in ["R", "G", "B"]:
            raw = exr.channel(ch, pt)
            channels.append(np.frombuffer(raw, dtype=np.float32).reshape(h, w))
        return np.stack(channels, axis=-1)

    elif ext == ".hdr":
        # PIL can read Radiance HDR
        img = Image.open(path)
        return np.array(img).astype(np.float32)

    else:
        # Try as regular image (LDR)
        img = Image.open(path).convert("RGB")
        return np.array(img).astype(np.float32) / 255.0


def tonemap(hdr: np.ndarray) -> np.ndarray:
    """Simple Reinhard tonemap: HDR float -> [0,255] uint8."""
    # Reinhard: color / (1 + color)
    mapped = hdr / (1.0 + hdr)
    # Gamma correction
    mapped = np.power(np.clip(mapped, 0, 1), 1.0 / 2.2)
    return (mapped * 255).astype(np.uint8)


def equirect_to_cubemap(equirect: np.ndarray, face_size: int) -> dict:
    """Convert equirectangular image to 6 cubemap faces."""
    h, w, _ = equirect.shape

    faces = {}

    # Standard cubemap face-to-direction formulas (Vulkan/OpenGL convention).
    # For pixel at (u, v) in [-1, 1]:
    #   +X: dir = ( 1, -v, -u)    -X: dir = (-1, -v,  u)
    #   +Y: dir = ( u,  1,  v)    -Y: dir = ( u, -1, -v)
    #   +Z: dir = ( u, -v,  1)    -Z: dir = (-u, -v, -1)
    #
    # We generate faces named by game coords (X=right, Y=forward, Z=up):
    #   right/left = +X/-X, front/back = +Y/-Y, top/bottom = +Z/-Z
    # The renderer maps these to Vulkan layers accordingly.

    def make_dirs(face, uu, vv):
        if face == "right":   return np.stack([ np.ones_like(uu), -vv, -uu], axis=-1)
        elif face == "left":  return np.stack([-np.ones_like(uu), -vv,  uu], axis=-1)
        elif face == "front": return np.stack([ uu,  np.ones_like(uu),  vv], axis=-1)
        elif face == "back":  return np.stack([ uu, -np.ones_like(uu), -vv], axis=-1)
        elif face == "top":   return np.stack([ uu, -vv,  np.ones_like(uu)], axis=-1)
        elif face == "bottom":return np.stack([-uu, -vv, -np.ones_like(uu)], axis=-1)

    for name in ["right", "left", "front", "back", "top", "bottom"]:
        u = np.linspace(-1, 1, face_size)
        v = np.linspace(-1, 1, face_size)
        uu, vv = np.meshgrid(u, v)

        dirs = make_dirs(name, uu, vv)

        # Normalize
        norms = np.linalg.norm(dirs, axis=-1, keepdims=True)
        dirs = dirs / norms

        # Convert direction to equirectangular UV
        # Spherical coords: theta (azimuth around Y), phi (elevation)
        x, y, z = dirs[:, :, 0], dirs[:, :, 1], dirs[:, :, 2]
        theta = np.arctan2(x, -y)  # azimuth: 0 at -Y, wraps around
        phi = np.arcsin(np.clip(z, -1, 1))  # elevation

        # Map to equirect UV [0,1]
        eq_u = (theta / (2 * np.pi) + 0.5) % 1.0
        eq_v = 0.5 - phi / np.pi

        # Sample equirect image (nearest neighbor for simplicity)
        px = np.clip((eq_u * w).astype(int), 0, w - 1)
        py = np.clip((eq_v * h).astype(int), 0, h - 1)

        faces[name] = equirect[py, px]

    return faces


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <input.exr|input.hdr> <output_dir> [--size 512]")
        sys.exit(1)

    input_path = sys.argv[1]
    output_dir = sys.argv[2]
    face_size = 512

    for i, arg in enumerate(sys.argv):
        if arg == "--size" and i + 1 < len(sys.argv):
            face_size = int(sys.argv[i + 1])

    if not os.path.exists(input_path):
        print(f"ERROR: Input file not found: {input_path}")
        sys.exit(1)

    os.makedirs(output_dir, exist_ok=True)

    print(f"Loading {input_path}...")
    equirect = load_hdr(input_path)
    print(f"  Size: {equirect.shape[1]}x{equirect.shape[0]}, dtype: {equirect.dtype}")

    print(f"Converting to {face_size}x{face_size} cubemap faces...")
    faces = equirect_to_cubemap(equirect, face_size)

    for name, face_data in faces.items():
        # Tonemap HDR -> LDR
        ldr = tonemap(face_data)
        img = Image.fromarray(ldr, "RGB").convert("RGBA")
        path = os.path.join(output_dir, f"{name}.png")
        img.save(path)
        print(f"  Saved {path}")

    print("Done!")


if __name__ == "__main__":
    main()
