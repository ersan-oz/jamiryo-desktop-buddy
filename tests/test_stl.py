"""STL export boundaries and lossless metadata sanitization."""
import json
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
from stl_mesh import PUBLIC_HEADER, inspect_mesh, public_stl
from check_public_release import audit_blob


def tetrahedron():
    a, b, c, d = (0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)
    triangles = [(a, c, b), (a, b, d), (b, c, d), (c, a, d)]
    return b'private metadata'.ljust(80, b' ') + struct.pack('<I', 4) + b''.join(
        struct.pack('<12fH', 0, 0, 0, *x, *y, *z, 0) for x, y, z in triangles)


class StlTests(unittest.TestCase):
    def test_geometry_is_preserved_byte_for_byte(self):
        original = tetrahedron()
        clean = public_stl(original)
        self.assertEqual(clean[:80], PUBLIC_HEADER)
        self.assertEqual(original[80:], clean[80:])
        self.assertTrue(inspect_mesh(clean, True)['two_faces_per_edge'])
        self.assertEqual(inspect_mesh(clean)['extents'], [1, 1, 1])

    def test_rejects_truncation_appended_content_and_bad_counts(self):
        data = public_stl(tetrahedron())
        for bad in [data[:83], data[:-1], data + b'private', data[:80] + struct.pack('<I', 0) + data[84:]]:
            with self.assertRaises(ValueError):
                inspect_mesh(bad)

    def test_rejects_nonfinite_coordinates_and_private_header(self):
        data = bytearray(public_stl(tetrahedron()))
        struct.pack_into('<f', data, 84 + 12, float('nan'))
        with self.assertRaises(ValueError):
            inspect_mesh(data)
        with self.assertRaises(ValueError):
            inspect_mesh(tetrahedron(), require_public_header=True)
        self.assertTrue(audit_blob('hardware/stl/head.stl', tetrahedron()))
        self.assertTrue(audit_blob('hardware/stl/unreviewed.stl', public_stl(tetrahedron())))

    def test_published_models_match_manifest(self):
        manifest = json.loads((ROOT / 'hardware/meshes.json').read_text())
        self.assertEqual(len(manifest['parts']), 3)
        for part in manifest['parts']:
            actual = inspect_mesh((ROOT / part['path']).read_bytes(), require_public_header=True)
            for field, expected in part['inspection'].items():
                self.assertEqual(actual[field], expected, f"Mesh changed: {part['path']} / {field}")


if __name__ == '__main__':
    unittest.main()
