# Printable enclosure parts

These three STL exports are the enclosure files supplied for the pictured
Jamiryo build. Native Fusion/STEP/parametric design sources are not included.

| Part | Download | Mesh extent X × Y × Z | Triangles |
| --- | --- | --- | --- |
| Head housing | [head.stl](stl/head.stl) | 60.00 × 45.00 × 65.00 | 2,302 |
| Cover | [cover.stl](stl/cover.stl) | 60.00 × 4.50 × 65.00 | 572 |
| Base | [base.stl](stl/base.stl) | 79.98 × 79.99 × 36.50 | 1,408 |

The intended slicer unit is **millimetres**. STL does not encode units; confirm
the dimensions after import. These are bounding-box extents of the supplied
meshes, not a calibrated dimensional drawing or an assembly fit guarantee.

## What was changed for publication

The files have descriptive English names and a generic 80-byte binary STL
header. Triangle counts, coordinates, normals and attribute bytes are unchanged.
No owner's file path or free-text CAD metadata is carried into that header.
The [inspection manifest](meshes.json) records hashes and mesh diagnostics.

## Mesh and printing notes

The cover and base have two incident faces per mesh edge in the automated check.
The head export contains **468 coincident duplicate faces and 182 nonmanifold
edges** when coordinates are rounded to five decimal places. Its geometry is
published as supplied; no automatic union, welding, hole filling or other mesh
repair has been applied. This check does not establish intersection-free geometry,
correct wall thickness or successful printing.

Inspect the head in your slicer's repair/preview tools before printing. If the
slicer requests a repair, review the resulting cavities and mounting features
before using that repaired copy. Check the OLED window, camera opening, speaker
and microphone clearance, servo mount, cable movement and battery fit against
your actual hardware. Keep your original design/export files separately.

Orient each part deliberately; the cover has a broad flat face, while the base
has a larger footprint. Supports, layer height, material, infill and tolerances
are not prescribed because a verified print profile is not supplied. There is
no complete exploded assembly drawing, screw specification or native CAD model
in this release.

For electrical connections and first motor testing, use the [hardware guide](../docs/hardware.md).
The STL meshes use the repository's [MIT license](../LICENSE).
