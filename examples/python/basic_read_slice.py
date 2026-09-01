import sys

import block3d


if len(sys.argv) < 2:
    raise SystemExit(f"usage: {sys.argv[0]} FILE.b3d")

reader = block3d.Reader(sys.argv[1])
info = reader.info()
print(info)

x0 = reader.read_slice("x", 0)
print("x0", x0.shape, x0.dtype, float(x0[0, 0]))

col = reader.read_column("x", 0, 0)
print("x column", col.shape, float(col[0]))
