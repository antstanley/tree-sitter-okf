# Changesets

Every user-facing change ships with a changeset: run `npx changeset`, choose
**patch**, and write a one-line summary. It becomes the CHANGELOG entry.

The version follows OKF: MAJOR.MINOR is the OKF version the grammar targets
(`okfVersion` in `package.json`), so a **minor** (or **major**) changeset is
only for supporting a new OKF version, together with updating `okfVersion`.
`script/check-versions.js` rejects anything else. Node types, fields and
query captures are API (spec P7): within one OKF version they are only
added. See `docs/releasing.md`.

Accumulated changesets are consumed by the "Version packages" PR that
`.github/workflows/version-pr.yml` maintains. Merging it bumps the version
in every manifest (`package.json`, `Cargo.toml`, `pyproject.toml`,
`tree-sitter.json`, `Makefile`, `CMakeLists.txt` and the lockfiles, via
`tree-sitter version`) and writes `CHANGELOG.md`. Tagging that commit
(`git tag vX.Y.Z && git push origin vX.Y.Z`) triggers the release to npm,
crates.io and PyPI (see `docs/releasing.md`).
