# SPDX-License-Identifier: Apache-2.0
"""Original geometry and authored-bit fixtures for native acceptance tests."""
import json
import math
import struct

def double_bits(value):
    return struct.unpack('<Q', struct.pack('<d', value))[0]

def scalar_bits(value, kind):
    if kind == 0:
        return struct.unpack('<I', struct.pack('<f', value))[0]
    if kind == 1:
        return double_bits(value)
    if kind == 2:
        return int(value) & 0xffffffff
    return int(value)

def channels(vertices, faces, corners):
    result = []
    for domain, rows in enumerate([vertices, faces, corners]):
        for kind in range(7):
            for ragged in [False, True]:
                values, offsets = [], [0]
                for row in range(rows):
                    count = (row % 4) * 2 if ragged else 2
                    for component in range(count):
                        value = (row * 7 + component * 3) % 251
                        if kind < 2:
                            value = (value - 120) / 8
                        if kind == 2:
                            value -= 125
                        if kind == 6:
                            value += 2**63 + 1
                        values.append(scalar_bits(value, kind))
                    offsets.append(len(values))
                # Both an authored and a missing NaN payload are opaque storage.
                if kind < 2 and len(values) > 4:
                    values[0] = 0x7fc01234 if kind == 0 else 0x7ff8000000001234
                    values[3] = 0x80000000 if kind == 0 else 0x8000000000000000
                result.append({'domain': domain, 'name': f'custom_{kind}_{int(ragged)}',
                               'semantic': 'CUSTOM', 'set': kind + 2, 'components': 2,
                               'type': kind, 'values': values, 'offsets': offsets if ragged else None,
                               'present': [int(row % 3 != 1) for row in range(rows)],
                               'metadata': {'association': 'original opaque rows', 'schema': 'v1'}})
    for set_index in [0, 1, 3]:
        result.append({'domain': 2, 'name': f'uv_{set_index}', 'semantic': 'TEXCOORD', 'set': set_index,
                       'components': 2, 'type': 0,
                       'values': [scalar_bits((c * 11 + axis + set_index) / 16, 0) for c in range(corners) for axis in range(2)],
                       'offsets': None, 'present': None,
                       'metadata': {'coordinate_system': 'authored'}})
    result.append({'domain': 1, 'name': 'material', 'semantic': 'MATERIAL', 'set': None,
                   'components': 1, 'type': 2, 'values': [f % 3 for f in range(faces)],
                   'offsets': None, 'present': None, 'metadata': {'asset_ref': 'fixture materials'}})
    for name, kind in [('joints', 4), ('weights', 1)]:
        values, offsets = [], [0]
        for row in range(vertices):
            for influence in range(row % 7):
                value = row + influence if name == 'joints' else (influence + 1) / 8
                values.append(scalar_bits(value, kind))
            offsets.append(len(values))
        result.append({'domain': 0, 'name': name, 'semantic': name.upper(), 'set': 0,
                       'components': 1, 'type': kind, 'values': values, 'offsets': offsets,
                       'present': [int(row % 4 != 2) for row in range(vertices)],
                       'metadata': {'pair': 'joints/weights', 'normalization': 'none'}})
    return result

def mesh(points, faces=None, attributes=False):
    faces = faces if faces is not None else [list(range(len(points)))]
    offsets, corners = [0], []
    for face in faces:
        corners += face
        offsets.append(len(corners))
    result = {'positions': [[double_bits(float(c)) for c in p] for p in points],
              'face_offsets': offsets, 'corner_vertices': corners,
              'attributes': channels(len(points), len(faces), len(corners)) if attributes else []}
    return result

def serialize(mesh, cancel_after=-1):
    faces = [mesh['corner_vertices'][a:b] for a, b in zip(mesh['face_offsets'], mesh['face_offsets'][1:])]
    lines = [f'{len(mesh["positions"])} {len(faces)}']
    lines += [' '.join(map(str, point)) for point in mesh['positions']]
    lines += [f'{len(face)} ' + ' '.join(map(str, face)) for face in faces]
    lines.append(str(len(mesh['attributes'])))
    for a in mesh['attributes']:
        quote = lambda value: json.dumps(value, ensure_ascii=False)
        lines.append(f'{a["domain"]} {quote(a["name"])} {quote(a["semantic"])} {a["set"] if a["set"] is not None else -1} {a["components"]} {a["type"]}')
        lines.append(f'{len(a["values"])} ' + ' '.join(map(str, a['values'])))
        for field in ['offsets', 'present']:
            rows = a[field]
            lines.append('0' if rows is None else f'1 {len(rows)} ' + ' '.join(map(str, rows)))
        lines.append(str(len(a['metadata'])))
        lines += [quote(k) + ' ' + quote(v) for k, v in sorted(a['metadata'].items())]
    lines.append(str(cancel_after))
    return '\n'.join(lines) + '\n'


def cases():
    fixtures = []
    shapes = [
        [(0,0),(4,0),(1,3)],
        [(0,0),(4,0),(4,4),(0,4)],
        [(0,0),(4,0),(4,4),(2,1),(0,4)],
        [(0,0),(6,0),(6,6),(4,6),(4,1),(2,1),(2,6),(0,6)],
        [(0,0),(1,0),(2,0),(4,0),(4,4),(0,4)],
    ]
    transforms = [lambda x,y:(x,y,0),lambda x,y:(0,x,y),lambda x,y:(x,0,y),lambda x,y:(2*x+y,-x+3*y,x-2*y)]
    for shape in shapes:
        for transform in transforms:
            points = [transform(x,y) for x,y in shape]
            for reversed_face in [False,True]:
                face = list(range(len(points)))
                if reversed_face: face.reverse()
                fixtures.append(mesh(points,[face],True))
    fixtures.append(mesh([(0,0,0),(5e-324,0,0),(0,5e-324,0)],attributes=True))
    fixtures.append(mesh([(0,0,0),(1,1,0),(2,math.nextafter(2,math.inf),0)],attributes=True))
    maximum=float.fromhex('0x1.fffffffffffffp1023')
    fixtures.append(mesh([(0,0,0),(maximum,0,0),(maximum,5e-324,0),(0,5e-324,0)],attributes=True))
    points=[(0,0,0),(4,0,0),(4,4,0),(0,4,0),(2,1,0),(8,0,0),(8,4,0)]
    faces=[[0,1,4],[1,5,6,2],[0,1,2,4,3]]
    fixtures.append(mesh(points,faces,True))
    fixtures.append(mesh(points,[list(reversed(f)) for f in faces],True))
    fixtures.append(mesh(points,faces*24,True))
    fixtures.append(mesh([],[],True))
    return fixtures
