"""Inspect binary STL geometry and replace only its free-text metadata header."""
import collections
import hashlib
import math
import struct

PUBLIC_HEADER = b'Jamiryo public STL | geometry preserved | no personal metadata'.ljust(80, b' ')
PUBLIC_MESHES = {'hardware/stl/head.stl', 'hardware/stl/cover.stl', 'hardware/stl/base.stl'}


def inspect_mesh(data, require_public_header=False):
    if len(data) < 84:
        raise ValueError('Truncated binary STL')
    count = struct.unpack_from('<I', data, 80)[0]
    if not 1 <= count <= 400000 or len(data) != 84 + count * 50:
        raise ValueError('STL triangle count/length mismatch')
    if require_public_header and data[:80] != PUBLIC_HEADER:
        raise ValueError('STL has an unreviewed metadata header')
    lower, upper = [math.inf] * 3, [-math.inf] * 3
    edges, faces = collections.Counter(), collections.Counter()
    degenerate = 0
    for i in range(count):
        values = struct.unpack_from('<12fH', data, 84 + 50 * i)
        if not all(math.isfinite(v) for v in values[:12]):
            raise ValueError('Non-finite STL normal or coordinate')
        raw = [tuple(values[3 + j*3 + k] for k in range(3)) for j in range(3)]
        for vertex in raw:
            lower = [min(a, b) for a, b in zip(lower, vertex)]
            upper = [max(a, b) for a, b in zip(upper, vertex)]
        tri = [tuple(round(v, 5) for v in vertex) for vertex in raw]
        ab = [raw[1][k] - raw[0][k] for k in range(3)]
        ac = [raw[2][k] - raw[0][k] for k in range(3)]
        cross = [ab[1]*ac[2]-ab[2]*ac[1], ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0]]
        degenerate += sum(v*v for v in cross) <= 1e-20
        faces[tuple(sorted(tri))] += 1
        for a, b in [(tri[0], tri[1]), (tri[1], tri[2]), (tri[2], tri[0])]:
            edges[tuple(sorted((a, b)))] += 1
    boundary, nonmanifold = sum(v == 1 for v in edges.values()), sum(v > 2 for v in edges.values())
    return {'bytes': len(data), 'triangles': count,
            'extents': [round(b-a, 5) for a, b in zip(lower, upper)],
            'boundary_edges': boundary, 'nonmanifold_edges': nonmanifold,
            'duplicate_faces': sum(v-1 for v in faces.values()),
            'degenerate_triangles': degenerate, 'edge_coordinate_rounding_decimals': 5,
            'two_faces_per_edge': not boundary and not nonmanifold,
            'triangle_payload_sha256': hashlib.sha256(data[84:]).hexdigest(),
            'file_sha256': hashlib.sha256(data).hexdigest()}


def public_stl(data):
    inspect_mesh(data)
    return PUBLIC_HEADER + data[80:]
