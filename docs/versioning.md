# Source and package version identity

| Field | Value |
|---|---|
| ID | GEOMETRY-SOURCE-VERSION-001 |
| Version | 0.1.1 |
| Status | Implemented development profile; release qualification separate |
| Owner | MeshvaleGeometry source/build/package identity |

MeshvaleGeometry resolves one native version for its CMake project and installed
CMake package. Untagged development uses `0.0.0`. Python's complete SCM/package version
remains a separate value; a dev/local string never becomes its stable numeric
prefix. The source rules below apply to this product independently of any outer
project or build directory.

## Own checkout and exact tags

An own checkout's canonical Git root must equal this source root, including a
proper worktree. An unrelated parent repository supplies no identity. Clean means
no tracked changes or nonignored untracked files; ignored generated data is allowed.
Own-root Git is authoritative over stale packaged metadata. A root .git marker
with unusable Git produces unverified development rather than an archive release.

In clean own-root Git, enumerate tags exactly at HEAD. Recognized stable names
have vX.Y.Z with decimal components. Strip leading zeroes from each component,
retaining a single `0` for zero: v01.2.3 and v1.2.3 have identity `1.2.3`. One unique
canonical stable identity owns the native version; equivalent aliases agree.
Different recognized stable identities at the same commit are an error.
Unrelated tags are ignored. Dirty/no recognized exact-tag source uses `0.0.0`.

## Git-free source metadata

The pre-export marker has these five fields exactly once:

```text
meshvale-project: meshvale-geometry
node: $Format:%H$
node-date: $Format:%cI$
describe-name: $Format:%(describe:tags=true,match=v[0-9]*)$
ref-names: $Format:%D$
```

The source .gitattributes selects the marker for Git substitution:

```text
.git_archival.txt export-subst
```

An unexpanded marker retains all four exact variable placeholders shown above
and the correct fixed product name. Partial expansion is malformed. A commit/tag
archive supplies actual values; an archive of a raw tree may leave the template
neutral. Keep one describe expression in the substituted marker. Expanded node,
describe and exact-ref identities must agree under the rules below.

A product-owned .git_archival.txt uses a fixed meshvale-project value matching
meshvale-geometry, plus node, node-date, describe-name and ref-names in the SCM archive
shape. Real git-archive export substitution carries commit/tag provenance; a
nearest tag or directory name does not establish an exact release. All-unexpanded
template fields are neutral. Expanded exact refs use the same unique canonical
tag rule; expanded development remains0.0.0. Partial/malformed/duplicate,
wrong-product or contradictory identity data is an error. Field labels are
case-sensitive; node is a complete 40- or 64-character hex commit identity.

PKG-INFO supplies a second product-named source-distribution channel. Read only
the header through its first empty line, with LF/CRLF. Name/Version labels are
case-insensitive, each exactly once and without continuation; trim surrounding
ASCII spaces/tabs. Normalize name separator runs dash/underscore/dot to dash and
ignore case, then require meshvale-geometry. Other headers and body text are ignored.
Missing/duplicate/empty/wrong-product identity headers are errors.

| Full version shape | Native interpretation |
|---|---|
| X.Y.Z | Canonical stable candidate |
| 0.0.0 without positive release provenance | Neutral development fallback |
| X.Y.Z.devN, optionally +local tokens | Supported nonrelease `0.0.0` |
| X.Y.Z+local tokens | Supported nonrelease `0.0.0` |
| a/b/rc, post, epoch or four release components | Unsupported native version profile; configuration error |
| Empty/garbage identity | Malformed; configuration error |

Digit groups are decimal. Local tokens are nonempty ASCII alphanumerics separated
by dot/dash/underscore; dev/post/a/b/rc classification ignores label case. This is
a bounded native metadata profile, not a general Python-version validity test.

Available archive and sdist release identities must agree. Positive development
archive data conflicts with nonzero stable PKG-INFO. An unexpanded neutral template
does not. Positive release data conflicts with dev/local/different or fallback-zero
package metadata. Compatible nonrelease channels retain native `0.0.0` without
asserting equality of their full Python strings. A raw copy without identity
metadata remains unversioned development.

## Native and Python build routes

Native identity resolution and a successful Python build are separate outcomes.
With the pinned SCM backend, expanded node-only/no-tag archives are supported
native development inputs but unsupported Python archive inputs: the backend can
fail its tag parsing before CMake. An unrelated numeric exact tag can similarly
preempt its describe selection. Preserve that actual error rather than inferring
a wheel from the native resolver result.

Nonignored untracked source makes the native checkout development, while SCM can
still produce stable FULL because it ignores untracked files. The consistency
rule then fails rather than claiming a matching release. Positive Python archive
checks require an unambiguous recognized exact tag or recognized-ancestor dev
history. This profile keeps the backend parser/describe/dirty behavior unchanged.

## Python build consistency

Use SKBUILD_PROJECT_VERSION_FULL only when normalized SKBUILD_PROJECT_NAME is this
distribution. Ignore unrelated/absent-name inherited inputs. Matching name without
FULL is an unsupported-backend error. The shortened SKBUILD_PROJECT_VERSION is
ignored, even when it differs from FULL. Apply the full-version table above.
Nonzero stable FULL needs matching positive source provenance; neutral0.0.0 is
allowed for unversioned development. A positive release requires matching stable
FULL when present. Backend values cross-check identity and never override it.

Native-only configuration needs no Python version parser. Current dependency
requirements remain owned by [CMakeLists.txt](../CMakeLists.txt) and Python build/
installation metadata by [pyproject.toml](../pyproject.toml). Runtime compatibility,
supported dependency combinations and released artifacts require their own tested
declarations.

## Check source metadata

With standard-library Python, Git and CMake 3.24 or newer available, run
[the source metadata probe](../scripts/check-source-version.py):

```sh
python scripts/check-source-version.py
```

The probe creates original temporary repositories and configures metadata
without a C++ compiler. These checks cover source identity; native binaries,
Python backend builds and supported release combinations need separate checks.
