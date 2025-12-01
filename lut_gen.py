import argparse
import numpy as np
import matplotlib.pyplot as plt


def generate_one(colormap: str, filename: str, symbol: str) -> None:
    N = 65536
    t = np.linspace(0.0, 1.0, N, dtype=np.float64)
    cmap = plt.get_cmap(colormap)
    # cmap(t) -> (R,G,B,A) in [0..1]
    rgba = cmap(t)
    rgb = (rgba[:, :3] * 255.0 + 0.5).astype(np.uint8)

    with open(filename, "w") as f:
        f.write("#pragma once\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"static const uint8_t {symbol}[65536][3] = {{\n")
        for i, (r, g, b) in enumerate(rgb):
            f.write(f"    /* {i:5d} */ {{ {r:3d}, {g:3d}, {b:3d} }},\n")
        f.write("};\n")


def main():
    parser = argparse.ArgumentParser(description="Generate GRAY16->RGB LUT headers")
    parser.add_argument("--palette", "-p", default="turbo",
                        help="Palette name: turbo (default), viridis, magma, jet, prism; alias: virdis->viridis")
    parser.add_argument("--all", action="store_true",
                        help="Generate headers for all palettes (turbo, viridis, magma, jet, prism)")
    args = parser.parse_args()

    if args.all:
        generate_one("turbo", "gray16_to_rgb_lut.h", "gray16_to_rgb")
        generate_one("viridis", "gray16_to_rgb_lut_viridis.h", "gray16_to_rgb_viridis")
        generate_one("magma", "gray16_to_rgb_lut_magma.h", "gray16_to_rgb_magma")
        generate_one("jet", "gray16_to_rgb_lut_jet.h", "gray16_to_rgb_jet")
        generate_one("prism", "gray16_to_rgb_lut_prism.h", "gray16_to_rgb_prism")
        return

    pal = (args.palette or "turbo").lower()
    if pal == "virdis":
        pal = "viridis"

    if pal == "turbo":
        generate_one("turbo", "gray16_to_rgb_lut.h", "gray16_to_rgb")
    elif pal == "viridis":
        generate_one("viridis", "gray16_to_rgb_lut_viridis.h", "gray16_to_rgb_viridis")
    elif pal == "magma":
        generate_one("magma", "gray16_to_rgb_lut_magma.h", "gray16_to_rgb_magma")
    elif pal == "jet":
        generate_one("jet", "gray16_to_rgb_lut_jet.h", "gray16_to_rgb_jet")
    else:
        raise SystemExit(f"Unknown palette: {args.palette}")


if __name__ == "__main__":
    main()
