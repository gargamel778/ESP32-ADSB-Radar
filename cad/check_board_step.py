#!/usr/bin/env python3
"""Re-reads cad/UEDX80480070E-WB-A_board.step and checks it: one named solid per part, sizes, the mounting-hole spacing, and writes front / back / side drawings
(SVG, and PNG if rsvg-convert is installed) to build/cad-check/ for comparing with VIEWE's mechanical drawing.   build/cadenv/bin/python cad/check_board_step.py"""
import os, subprocess
from build123d import import_step, Compound, ExportSVG, Unit, LineType, Axis, GeomType, Vector, Plane, Color

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
shape = import_step(os.path.join(here, "UEDX80480070E-WB-A_board.step"))
solids = list(shape.children)   # the parts keep their names and colours in the file
print(f"{len(solids)} named parts")
for s in solids:
    bb = s.bounding_box()
    print(f"  {str(s.label or '?'):28s} {bb.size.X:7.2f} x {bb.size.Y:7.2f} x {bb.size.Z:5.2f}   X {bb.min.X:8.2f}..{bb.max.X:7.2f}  Y {bb.min.Y:7.2f}..{bb.max.Y:7.2f}  Z {bb.min.Z:6.2f}..{bb.max.Z:5.2f}  vol {s.volume:9.1f}")
pcb = [s for s in solids if str(s.label) == "PCB"][0]
holes = [f for f in pcb.faces() if f.geom_type == GeomType.CYLINDER]
mount = [f.bounding_box() for f in holes if abs(f.bounding_box().size.X - 4.10) < 0.05 or abs(f.bounding_box().size.Y - 4.10) < 0.05]
span_x = max(b.max.X for b in mount) - min(b.min.X for b in mount) - 4.10
span_y = max(b.max.Y for b in mount) - min(b.min.Y for b in mount) - 4.10
print(f"mounting-hole spacing: {span_x:.2f} x {span_y:.2f}  (drawing: 173.91 x 98.47)")
print(f"small holes (2.54 mm pad rows): {sum(1 for f in holes if abs(f.bounding_box().size.X - 1.0) < 0.05)}")

out = os.path.join(root, "build", "cad-check"); os.makedirs(out, exist_ok=True)
views = {"front": ((0, 0, 100), (0, 1, 0)), "back": ((0, 0, -100), (0, 1, 0)), "side": ((100, 0, 0), (0, 0, 1)), "top": ((0, 100, 0), (0, 0, 1))}
for name, (origin, up) in views.items():
    vis, hid = shape.project_to_viewport(origin, up)
    exp = ExportSVG(unit=Unit.MM, scale=3, margin=5, line_weight=0.15)
    exp.add_layer("visible", line_weight=0.2); exp.add_layer("hidden", line_type=LineType.ISO_DASH, line_weight=0.1)
    exp.add_shape(vis, layer="visible")
    svg = os.path.join(out, f"{name}.svg"); exp.write(svg)
    subprocess.run(["rsvg-convert", "-b", "white", "-o", svg[:-4] + ".png", svg], check=False)
print("drawings in", out)
