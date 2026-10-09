# Workflow report envelopes

| Field | Value |
|---|---|
| ID | GEO-REPORT-001 |
| Version | 0.1.0 |
| Status | Development contract; no released workflow producer |
| Owner | Declarative envelopes, common consistency rules and reference validation |

The [JSON Schema source](../python/meshvale_reports/report-v1.schema.json) owns exact fields, types and version identifiers: `meshvale.report/1` for one asset and `meshvale.batch/1` for a batch. Both use JSON Schema Draft 2020-12; all references are internal. The [reference validator](../python/meshvale_reports/__init__.py) implements the additional consistency rules below. Package versions and report schema versions are independent. There is no stable release yet.

## Independent outcomes and scoped references

A single report includes producing tool/version, opaque input identity and optional content hash, resolved operation/options/targets, execution, candidate, profile, snapshots, diagnostics, coverage, correspondence, preservation/conversion, publication and extensions. Null input or operation is reserved for import/invocation failure. Applications own operation option meanings, predicate definitions and resolved target relationships; target rows may be out of range to describe a rejected request.

Execution is `completed`, `failed` or `cancelled`, with a stage and failure/cancellation reason. Candidate is `accepted`, `unchanged`, `rejected` or `not-attempted`. Profile is `passed`, `failed` or `incomplete`. Publication separately says whether a requested output was verified and finalized. An accepted candidate followed by an export failure is an execution failure with its candidate acceptance retained. A report-write failure after publication can retain the published output and still fail execution at stage `report`. A report validator cannot establish that files exist or match declared hashes; a real producer must supply that evidence.

Each declared snapshot is an asset state with local mesh identities and row counts. At most one snapshot has each input/candidate/output role. Mesh identity is scoped to a snapshot; an element reference additionally names its domain and row. Snapshot indices are neither durable edit handles nor indices in another snapshot. Diagnosed invalid *reference values* belong in diagnostic `data`; an optional diagnostic element identifies an existing offending row. All identities are opaque tokens, not filesystem locators. Producers must keep real machine paths out of published fixtures and evidence.

Correspondence maps name source and destination snapshot/mesh pairs and a vertex/face/corner domain. Maps point to a later role. `identity` has equal counts and null entries. `explicit` has one row per source element: a destination integer, null for deletion, or a nonempty list of unique destination integers for one-to-many correspondence. Multiple source rows may share a destination. This supports duplicate removal and polygon-to-triangle conversion without implying that triangles retain authored polygon loops. Bounds are checked against the named destination, not the source. Accepted/unchanged candidates need correspondence for every input mesh domain; unchanged uses identity maps. Mapping validity checks shape, references and bounds, not geometric or attribute truth.

Row counts and indices are canonical JSON integers in uint64 range; set indices are uint32. Python validation rejects float/bool substitutes for element indices and counts. Consumers must parse these integers exactly, including values above the JavaScript safe-number range; JSON Schema's mathematical integer type alone does not enforce lexical integer representation. Optional operation numbers may be finite floats. No UV-set or skin-influence count is imposed by the envelope. Per-channel claims name their domain/name/set; variable skin row preservation is a declared feature, not a claim inferred from counts. Claimed mesh preservation/conversion references maps with matching scopes/domains. Converted, unsupported and not-checked features require reasons.

## Coverage and profile evaluation

Coverage IDs distinguish check instances, with check/version, scope, diagnostics and a state. `performed` requires an explicit `passed`/`failed` predicate finding. `unsupported`, `skipped` and `failed` carry a reason and no predicate finding. Descriptive inspections and boolean policy predicates are distinct: completing a component traversal does not prove a single connected component, and native `performed` does not imply global mesh validity.

`coverage_state(native_status, finding=..., reason=...)` translates Geometry's native coverage: `performed` requires the caller's actual predicate finding; `blocked` becomes `skipped` with the blocking reason; `unsupported` retains that status/reason. Checker execution failures can be reported directly as `failed`. Applications own the predicate definition/version and derive findings from actual inspection data. Raw topology diagnostic dictionaries are not complete reports.

A profile lists coverage IDs that it requires. Missing references invalidate the report. Any required performed failure makes the profile `failed`; otherwise any required unperformed check makes it `incomplete`; otherwise it is `passed`. Optional unsupported/failed checks stay visible without failing a narrow profile. An empty requirement list passes vacuously and promises no checks. Producers cannot declare a different result from these rules. Diagnostics alone do not determine policy outcomes.

Successful publication requires an accepted/unchanged candidate, passed profile, output snapshot and verified artifact hashes. An unpublished result cannot claim finalized artifacts. A completed successful repair/conversion cannot leave its requested export unfinished. An inspection operation makes no edit/export and can complete with `not-attempted` candidate. Rejected candidates may include a separately identified diagnostic snapshot; they cannot be published by that repair operation.

## Batch and exit rules

Each batch job has a unique job ID, its input identity and either an attempted single-asset report or a not-attempted reason. Completed `continue` batches account for every job. Completed `fail-fast` batches can leave jobs unattempted only when an attempted job failed; interrupted batches retain reasons for unscheduled work. The envelope does not prescribe scheduling order or establish a running CLI implementation.

Single-asset exit meanings are 130 for cancellation, 2 for execution failure, 1 for rejected/unattempted edits or failed/incomplete required profiles, and 0 for completed successful work. A successful inspection needs no candidate. Batch precedence is **130, then 2, then 1, then 0**, including batch-level cancellation/failure such as report-write failure. It is not numeric maximum. The declared batch exit must agree with its per-job and batch execution outcomes.

## Consume independently

Native installations export `MeshvaleGeometry_REPORT_SCHEMA` from `find_package(MeshvaleGeometry CONFIG REQUIRED)`. It names the installed `share/MeshvaleGeometry/schemas/report-v1.schema.json`; the [installed consumer](../examples/consumer/CMakeLists.txt) reads it without Python. The same single source is copied into the Python wheel as `meshvale_reports/report-v1.schema.json`.

```sh
python -m pip install 'meshvale-geometry[reports]'
python -m unittest discover -s tests/reports -v
```

These are development consumption instructions; the distribution is not published yet. `import meshvale_reports` and `schema_document()` use the standard library, load no native Geometry extension and require no validator dependency. `validate`, `load`, `profile_outcome` and `exit_code` lazily require the optional `reports` extra (`jsonschema==4.26.0`, with its own dependencies). Native-only builds acquire no Python dependency. Original code/schema remains Apache-2.0; installed third-party validation dependencies retain their own licenses and are not vendored here.

```python
from meshvale_reports import load, exit_code, schema_document

schema = schema_document()
with open('examples/reports/accepted.json', encoding='utf-8') as source:
    report = load(source.read())
assert exit_code(report) == 0
```

The [example report](../examples/reports/accepted.json) is an original illustrative envelope fixture with a placeholder artifact hash. It is not output from a released repair/export command and does not prove actual publication or preservation. `validate` checks strict JSON values, structure and consistency without modifying caller data. `load` rejects duplicate JSON keys/nonfinite values before validation. Invalid declarations raise `ReportError`; a missing optional validator dependency raises `ImportError`. `profile_outcome` validates a detached copy with its computed outcome; `exit_code` requires a consistent report or batch declaration.

## Evolution and limits

Core fields are closed; missing/unknown fields and unsupported schema versions are refused. Applications use dotted namespaces under `extensions` for JSON object data, without redefining core meanings. Unknown namespaces are structurally accepted; their product owners must validate extension semantics. Future incompatible wire or common semantic changes require a new report schema identifier. Compatibility tests retain version-1 fixtures and exercise rejection of future versions and malformed references.

No runtime mesh payload, numerical preservation proof, general solid-validity guarantee, filesystem verification, durable handle lifetime, cancellation engine, parallel scheduler or actual API/CLI producer is supplied by this schema. Its tests establish declarative/reference behavior; products must test real reports against real operations and published artifacts. The dialect follows the [JSON Schema core specification](https://json-schema.org/draft/2020-12/json-schema-core); the optional validator follows [jsonschema validation documentation](https://python-jsonschema.readthedocs.io/en/stable/validate/).
