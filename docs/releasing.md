# Releasing

## Versioning

The parser follows [semantic versioning](https://semver.org/). Public node
types, fields and query captures are API (spec P7):

| Change | Bump |
|---|---|
| Rename or remove a node type, field or query capture | **major**, with a node-rename table in the changeset |
| Add a node type, field or capture; move a construct out of `yaml_unsupported` into the OKF-YAML subset | **minor** |
| A parse fix that leaves the node inventory unchanged; docs; helpers | **patch** |

Each release also records the OKF version it targets and the query-library
version (`queries/okf/`, which follows the OKF version) in its CHANGELOG
entry.

One version number covers every package: npm, crates.io and PyPI publish the
same version, and Go and Swift consumers use the matching `vX.Y.Z` tag. It
lives in `package.json`, `package-lock.json`, `tree-sitter.json`,
`Cargo.toml`, `Cargo.lock`, `pyproject.toml`, `Makefile` and
`CMakeLists.txt`. `script/check-versions.js` (run by `script/test` and by
the release workflow) fails if they disagree.

## Day to day

1. **Every user-facing change comes with a changeset.** Run `npx changeset`,
   choose the bump, and write the CHANGELOG line. Commit the generated
   `.changeset/*.md` with the change.
2. **The "Release: version packages" PR** is kept up to date by
   `.github/workflows/version-pr.yml` on every push to `main`. It runs
   `script/version-packages`: `changeset version` (bumps `package.json`,
   writes `CHANGELOG.md`), then `tree-sitter version` (carries the bump into
   every other manifest and lockfile).
3. **Merge that PR** when you want to release. GitHub does not run workflows
   on pushes made with the Actions token, so CI does not run on the PR
   itself. It runs on `main` after the merge, and again at release time.
4. **Tag the merge commit:**

   ```sh
   git pull
   git tag v1.2.3
   git push origin v1.2.3
   ```

5. `.github/workflows/release.yml` then does the following:
   - Checks that the tag matches every manifest.
   - Runs the full CI workflow on the tagged commit.
   - Builds the npm tarball, the Python sdist and abi3 wheels (Linux x86-64
     and arm64, glibc and musl; macOS x86-64 and arm64; Windows).
   - Publishes to all three registries by trusted publishing.
   - Creates the GitHub Release from the CHANGELOG entry.
6. **Approve the npm upload.** npm publishes are *staged*: the job summary
   lists the staging id. Approve it with 2FA using `npm stage approve <id>`,
   or in the staged-packages page on npmjs.com. crates.io and PyPI are live
   as soon as their jobs finish.

A failed or partial release can be re-run from the Actions UI. Each publish
job skips a registry that already has the version.

**Dry run:** *Actions → Release → Run workflow* on `main` runs validation, CI
and every build without publishing. Use it before tagging when the build
matrix or packaging has changed.

## One-time setup

No registry token is stored anywhere. Each registry trusts this repository's
`release.yml` workflow, running in the `publish` environment, and exchanges
its GitHub OIDC token for a short-lived credential.

### GitHub

Create an environment named **`publish`** (*Settings → Environments*).
Adding required reviewers there puts a manual approval in front of every
publish job. Restricting its deployment tags to `v*` stops branches from
using it.

### npm

A trusted publisher can only be added to a package that already exists on
npm, so the first version is published by hand:

```sh
npm login
npm publish --access public            # from a clean checkout of main at 1.0.0
npm trust github tree-sitter-okf \
  --repo antstanley/tree-sitter-okf \
  --file release.yml \
  --env publish \
  --allow-stage-publish
```

`--allow-stage-publish` (without `--allow-publish`) limits CI to *staging*
releases. Nothing goes live without a maintainer's 2FA approval. Trusted
publishing needs npm ≥ 11.5.1, and the workflow installs the latest npm.
`repository.url` in `package.json` must match the GitHub repository exactly.

### crates.io

crates.io also requires the crate to exist before a trusted publisher can
be configured:

```sh
cargo login                            # a temporary API token
cargo publish                          # from a clean checkout of main at 1.0.0
```

Then, in the crate's settings on crates.io, add a trusted publisher with
repository owner `antstanley`, repository `tree-sitter-okf`, workflow
`release.yml` and environment `publish`, and revoke the temporary token.

### PyPI

PyPI can create a project on its first trusted publish. On PyPI, open
*Your projects → Publishing → Add a new pending publisher* and enter:

| Field | Value |
|---|---|
| PyPI project name | `tree-sitter-okf` |
| Owner | `antstanley` |
| Repository name | `tree-sitter-okf` |
| Workflow name | `release.yml` |
| Environment name | `publish` |

The first tagged release then creates the project.

### First release (1.0.0)

Do the steps in this order. The tag must come last, or the npm and crates.io
jobs fail for lack of a trusted publisher.

1. Create the `publish` environment and the PyPI pending publisher.
2. Publish 1.0.0 to npm and crates.io by hand, as above, and add their
   trusted publishers.
3. Push the tag: `git tag v1.0.0 <commit> && git push origin v1.0.0`. The
   workflow skips npm and crates.io (1.0.0 is already there), creates the
   PyPI project, and cuts the GitHub Release.

### Go and Swift

Nothing to set up. `go get github.com/antstanley/tree-sitter-okf@vX.Y.Z`
and SwiftPM resolve the git tag directly.
