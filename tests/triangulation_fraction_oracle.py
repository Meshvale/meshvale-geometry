# SPDX-License-Identifier: Apache-2.0
"""Independent rational geometry verification; it never constructs a triangulation."""
import collections
import copy
from fractions import Fraction
import itertools
import struct

def coordinates(mesh):
    return [tuple(Fraction.from_float(struct.unpack('<d', struct.pack('<Q', b))[0]) for b in point) for point in mesh['positions']]

def orient(a, b, c):
    return (b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0])

def area2(poly):
    return sum(a[0]*b[1]-a[1]*b[0] for a, b in zip(poly, poly[1:]+poly[:1]))

def project(poly):
    normal = None
    for a, b, c in itertools.combinations(poly, 3):
        u, v = [x-y for x, y in zip(b, a)], [x-y for x, y in zip(c, a)]
        trial = [u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0]]
        if any(trial):
            normal = trial
            break
    assert normal is not None, 'no exact plane'
    assert all(sum(n*(x-y) for n, x, y in zip(normal, p, poly[0])) == 0 for p in poly), 'nonplanar represented coordinates'
    drop = max(range(3), key=lambda axis: abs(normal[axis]))
    return [tuple(v for axis, v in enumerate(p) if axis != drop) for p in poly]

def on_segment(a, b, p):
    return orient(a, b, p) == 0 and all(min(x,y) <= z <= max(x,y) for x, y, z in zip(a,b,p))

def inside(poly, p):
    if any(on_segment(a,b,p) for a,b in zip(poly, poly[1:]+poly[:1])):
        return True
    winding = 0
    for a,b in zip(poly, poly[1:]+poly[:1]):
        if a[1] <= p[1] < b[1] and orient(a,b,p) > 0:
            winding += 1
        if b[1] <= p[1] < a[1] and orient(a,b,p) < 0:
            winding -= 1
    return winding != 0

def proper_cross(a,b,c,d):
    return orient(a,b,c)*orient(a,b,d) < 0 and orient(c,d,a)*orient(c,d,b) < 0

def triangle_intersection_area(a, b):
    # Independent exact polygon clipping uses constructed Fraction intersections.
    # Its output is used only as proof, never as conversion implementation.
    if area2(b) < 0:
        b = list(reversed(b))
    subject = list(a)
    for p,q in zip(b, b[1:]+b[:1]):
        old, subject = subject, []
        if not old:
            return Fraction(0)
        for start,end in zip(old[-1:]+old[:-1], old):
            ds, de = orient(p,q,start), orient(p,q,end)
            if (ds >= 0) != (de >= 0):
                t = ds/(ds-de)
                subject.append(tuple(x+t*(y-x) for x,y in zip(start,end)))
            if de >= 0:
                subject.append(end)
    return abs(area2(subject))/2 if len(subject) >= 3 else Fraction(0)

def expected_channel(source, rows):
    result = copy.deepcopy(source)
    result['values'] = []
    result['offsets'] = [0] if source['offsets'] is not None else None
    result['present'] = [] if source['present'] is not None else None
    for row in rows:
        begin,end = (source['offsets'][row:row+2] if source['offsets'] is not None else
                     (row*source['components'], (row+1)*source['components']))
        result['values'] += source['values'][begin:end]
        if result['offsets'] is not None:
            result['offsets'].append(len(result['values']))
        if result['present'] is not None:
            result['present'].append(source['present'][row])
    return result

def verify(source, result):
    assert result['source'] == source and result['source_unchanged'] is True
    candidate = result['mesh']
    assert candidate['positions'] == source['positions']
    faces = source['face_offsets']
    source_coords = coordinates(source)
    output_faces = candidate['face_offsets']
    assert output_faces == list(range(0, len(candidate['corner_vertices'])+1, 3))
    assert len(result['corner_sources']) == len(candidate['corner_vertices'])
    assert len(result['face_sources']) == len(output_faces)-1
    assert len(result['face_output_offsets']) == len(faces)
    assert result['face_output_offsets'][0] == 0
    assert result['face_output_offsets'][-1] == len(result['face_sources'])
    triangle_count = 0
    for face,(start,end) in enumerate(zip(faces, faces[1:])):
        polygon = project([source_coords[v] for v in source['corner_vertices'][start:end]])
        out_start,out_end = result['face_output_offsets'][face:face+2]
        assert out_end-out_start == end-start-2
        polygon_area = area2(polygon)
        assert polygon_area != 0
        triangles, edge_counts = [], collections.Counter()
        for triangle in range(out_start, out_end):
            assert result['face_sources'][triangle] == face
            corners = result['corner_sources'][3*triangle:3*triangle+3]
            assert all(start <= corner < end for corner in corners)
            assert [source['corner_vertices'][corner] for corner in corners] == candidate['corner_vertices'][3*triangle:3*triangle+3]
            points = [polygon[corner-start] for corner in corners]
            assert area2(points)*polygon_area > 0, 'triangle winding/degeneracy'
            assert inside(polygon, tuple(sum(p[axis] for p in points)/3 for axis in range(2))), 'triangle centroid outside'
            for a,b in zip(points, points[1:]+points[:1]):
                assert inside(polygon, tuple((x+y)/2 for x,y in zip(a,b))), 'triangle edge leaves polygon'
                assert not any(proper_cross(a,b,c,d) for c,d in zip(polygon, polygon[1:]+polygon[:1])), 'triangle crosses polygon boundary'
            triangles.append(points)
            for a,b in zip(corners,corners[1:]+corners[:1]):
                edge_counts[a,b] += 1
        boundary = {(c, start+(c-start+1)%(end-start)) for c in range(start,end)}
        for a,b in boundary:
            assert edge_counts[a,b] == 1 and edge_counts[b,a] == 0, 'authored boundary/winding not retained'
        for edge,count in edge_counts.items():
            if edge not in boundary:
                assert count == 1 and edge_counts[edge[::-1]] == 1, 'interior correspondence not paired'
        assert sum(area2(t) for t in triangles) == polygon_area, 'coverage area changed'
        for a,b in itertools.combinations(triangles,2):
            assert triangle_intersection_area(a,b) == 0, 'triangle interiors overlap'
        triangle_count += len(triangles)
    assert len(source['attributes']) == len(candidate['attributes'])
    rows_by_domain = [list(range(len(source['positions']))), result['face_sources'], result['corner_sources']]
    for src,dst in zip(source['attributes'], candidate['attributes']):
        assert dst == expected_channel(src, rows_by_domain[src['domain']]), f'channel backing/type/missingness/metadata changed: {src["name"]}'
    return {'triangles': triangle_count, 'channels': len(source['attributes']),
            'coverage_fraction_exact': True, 'interior_overlap_area_zero': True,
            'authored_boundary_winding_exact': True, 'all_scalar_bits_and_correspondence_exact': True}
