# Changesets

Every user-facing change ships with a changeset: run `npx changeset`, pick
the semver impact, and write a one-line summary. It becomes the CHANGELOG
entry. Node types, fields and query captures are API (spec P7): renaming or
removing one is **major**, adding one is **minor**, and a parse fix that
leaves the node inventory unchanged is **patch**. A major changeset must
include a node-rename table.

Accumulated changesets are consumed by the "Version packages" PR that
`.github/workflows/version-pr.yml` maintains. Merging it bumps the version
in every manifest (`package.json`, `Cargo.toml`, `pyproject.toml`,
`tree-sitter.json`, `Makefile`, `CMakeLists.txt` and the lockfiles, via
`tree-sitter version`) and writes `CHANGELOG.md`. Tagging that commit
(`git tag vX.Y.Z && git push origin vX.Y.Z`) triggers the release to npm,
crates.io and PyPI (see `docs/releasing.md`).
