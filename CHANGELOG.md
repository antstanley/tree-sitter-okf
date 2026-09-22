# Changelog

All notable changes to `tree-sitter-okf`. The parser follows
[semantic versioning](https://semver.org/). Public node types, fields and
query captures are API: renaming or removing one is a major release, and
every such release has a node-rename table here (spec P7, §10.3).

Each release records three versions:

* **parser**: this package's version.
* **OKF**: the Open Knowledge Format version the grammar targets.
* **query library**: the version of `queries/okf/`, which follows the OKF
  version.

## 1.0.0 — 2026-09-22

parser **1.0.0** · OKF **0.2** (spec @ `ad30107`) · query library **0.2.0** ·
tree-sitter ABI **14** (generated with tree-sitter CLI 0.25)

First release. It implements `docs/grammar-spec.md` milestones M0–M5.

### Grammar

* One grammar for the whole document: `source_file` with optional
  `frontmatter` and `body` fields.
* **Frontmatter:** the OKF-YAML subset (`docs/okf-yaml.md`), handled by an
  indentation-aware external scanner. It covers block and flow collections,
  zero-indented sequences, plain, quoted and block scalars, anchors, tags,
  aliases, comments and directives. Anything outside the subset becomes
  `yaml_unsupported`, never `ERROR`. An unterminated block gets a
  `MISSING "---"`.
* **Body:** tree-sitter-markdown's block and inline grammars
  (`a0a00f8`) merged into one grammar, with GFM pinned on (pipe tables, task
  lists, strikethrough) and GFM footnotes added. The deltas are listed in
  `vendor/tree-sitter-markdown/MERGE.md`. Node names match upstream
  (`docs/node-types.md`).
* Experimental dialects, off by default: `OKF_DIALECT_WIKILINK=1` and
  `OKF_DIALECT_TAGS=1` at generate time.

### Queries

`highlights.scm`, `injections.scm`, `injections-fullyaml.scm` (opt-in:
frontmatter as `yaml`), `locals.scm`, `tags.scm`, and the OKF library
`okf/{fields,links,citations,sources,scalars,sections,computation}.scm`.

### Bindings

C, Go, Node, Python, Rust and Swift. The Node and Python bindings ship the
host helpers (`docs/host-helpers.md`): `classify`, `conceptId`,
`frontmatter`, trust tier, lifecycle status, staleness, citations join and
conformance findings. Shared fixtures keep the two implementations in
agreement.

### Verification

* 151 corpus cases.
* 646 of 651 upstream tree-sitter-markdown cases give identical trees. The
  other 5 are recorded divergences.
* Frontmatter scalar values match PyYAML on 113 blocks.
* 48 query fixtures and 18 host-helper cases.
* A property suite checks: no `ERROR` on fuzzed inputs, lossless leaves,
  incremental parse equals full parse, CRLF/BOM variants, and linear time.
* Benchmark (Apple M-series): about 3,600 fixture files/s. A 100 KB document
  parses in about 18 ms. A one-character edit re-parses in under 1 ms on
  every fixture document.

### Known limitations

* A pipe table whose header row has no leading `|` and starts with an
  emphasis delimiter (`*a* | b`) is not recognised as a table. It parses
  as a paragraph, without `ERROR`. Adding the leading `|` works around it.
* Frontmatter is a subset of YAML 1.2. Constructs outside it are opaque
  `yaml_unsupported` nodes, and are listed in `docs/okf-yaml.md`.
* `swift test` needs Xcode (XCTest). Without it, only `swift build` was
  verified.
