# tree-sitter-okf

A [tree-sitter](https://tree-sitter.github.io/) grammar for
[Open Knowledge Format (OKF)](https://github.com/GoogleCloudPlatform/open-knowledge-format)
v0.2 documents: markdown with YAML frontmatter, organised into knowledge
bundles.

**Status: 1.0.0.** It implements the whole
[grammar specification](docs/grammar-spec.md) (milestones M0–M5). See
[`CHANGELOG.md`](CHANGELOG.md) for what is verified and the known
limitations.

## Why a grammar of its own?

`.md` files in an OKF bundle are more than markdown:

1. **The document shape is OKF's.** A frontmatter block plus a body, with
   rules about where frontmatter is and isn't allowed.
2. **The profile is OKF's.** GFM tables (for `# Schema`), footnotes (for
   per-claim attribution), fenced and indented code, and cross-links.
   `tree-sitter-markdown` has no footnotes and gates GFM behind build flags.
3. **The semantic join crosses the frontmatter/body boundary.** Frontmatter
   `sources[].id` is joined to footnote labels in the body. Only a single
   tree can express that in one query.

So this grammar parses the whole document into one tree:

```markdown
---
type: Metric
tags: [finance]
sources:
- id: gl
  resource: /tables/gl
---

# Gross Margin

Revenue minus COGS.[^gl]

[^gl]: General ledger.
```

```query
(source_file
  frontmatter: (frontmatter
    (block_mapping
      (block_mapping_pair key: (plain_scalar) value: (plain_scalar))
      (block_mapping_pair key: (plain_scalar) value: (flow_sequence (plain_scalar)))
      (block_mapping_pair key: (plain_scalar)
        value: (block_sequence
          (block_sequence_item
            (block_mapping
              (block_mapping_pair key: (plain_scalar) value: (plain_scalar))
              (block_mapping_pair key: (plain_scalar) value: (plain_scalar))))))))
  body: (body
    (section
      (atx_heading (atx_h1_marker) heading_content: (inline))
      (paragraph (inline (footnote_reference label: (footnote_label))))
      (footnote_definition label: (footnote_label) (paragraph (inline))))))
```

* **Frontmatter** is parsed natively as [OKF-YAML](docs/okf-yaml.md), a
  documented subset of YAML. Anything outside it becomes a
  `yaml_unsupported` node, never an `ERROR`. A missing closing `---` is a
  `MISSING "---"`, so tools can say exactly what is wrong.
* **The body** uses the rules of
  [tree-sitter-markdown](https://github.com/tree-sitter-grammars/tree-sitter-markdown),
  with block and inline merged into one grammar. GFM is always on, footnotes
  are added, and node names match upstream. Existing markdown queries and
  habits carry over. The deltas are listed in
  [`vendor/tree-sitter-markdown/MERGE.md`](vendor/tree-sitter-markdown/MERGE.md).
* **Every node type** is documented in [`docs/node-types.md`](docs/node-types.md).

## Installing

| Ecosystem | Package | Language |
|---|---|---|
| Node | `tree-sitter-okf` (npm) | `require('tree-sitter-okf')` |
| Rust | `tree-sitter-okf` (crates.io) | `tree_sitter_okf::LANGUAGE` |
| Python | `tree-sitter-okf` (PyPI) | `tree_sitter_okf.language()` |
| Go | `github.com/antstanley/tree-sitter-okf/bindings/go` | `tree_sitter_okf.Language()` |
| Swift | `TreeSitterOkf` (SwiftPM) | `tree_sitter_okf()` |
| C | `make && make install` (`libtree-sitter-okf`, `tree-sitter-okf.pc`) | `tree_sitter_okf()` |

The packages are not published yet. Until they are, install from this
repository (for example `npm install github:antstanley/tree-sitter-okf`,
or `pip install git+https://github.com/antstanley/tree-sitter-okf`).

The generated parser uses tree-sitter **ABI 14** (spec D12). That is the
highest ABI Neovim 0.10 loads, and every current runtime supports it.

```js
const Parser = require('tree-sitter');
const OKF = require('tree-sitter-okf');

const parser = new Parser();
parser.setLanguage(OKF);
const tree = parser.parse(source);
console.log(tree.rootNode.toString());
```

```python
import tree_sitter, tree_sitter_okf

parser = tree_sitter.Parser(tree_sitter.Language(tree_sitter_okf.language()))
tree = parser.parse(source.encode())
```

## Queries

| File | Purpose |
|---|---|
| `queries/highlights.scm` | Highlighting, with `@markup.*` capture names (Neovim, Helix) |
| `queries/injections.scm` | Fenced code by info string, HTML. Frontmatter is native, not injected |
| `queries/injections-fullyaml.scm` | Opt-in: injects the frontmatter as `yaml` for full-YAML hosts |
| `queries/locals.scm` | Heading scopes (outlines, folding) |
| `queries/tags.scm` | Concept definitions and cross-link references |
| `queries/okf/fields.scm` | One capture per well-known frontmatter key (`@okf.field.type`, …) |
| `queries/okf/links.scm` | Link classification: URI, bundle-relative, relative, fragment |
| `queries/okf/sources.scm` | `sources[]` entries and their fields |
| `queries/okf/citations.scm` | Footnote labels, for the `sources[].id` join |
| `queries/okf/scalars.scm` | A replaceable typing policy for scalars (integer, timestamp, boolean) |
| `queries/okf/sections.scm` | Conventional headings and `log.md` date headings |
| `queries/okf/computation.scm` | The `# Computation` heading → code block pairing |

The OKF queries use only node patterns and `#match?`, so they give the same
results in every host.

## Host helpers

Some OKF facts need something the parse tree does not have: a filename, a
clock, the absence of a key, or what a value means. The Node and Python
bindings ship reference helpers for them. The helpers classify documents,
compute concept ids, turn frontmatter into a plain value, and report trust
tier, lifecycle status and staleness. They also join citations to footnotes
and produce OKF conformance findings.

```js
const { okf } = require('tree-sitter-okf');
okf.classify('metrics/gross-margin.md', tree); // 'concept'
okf.trustTier(tree);                           // 'human-reviewed'
okf.citations(tree).unresolved;                // footnotes with no source
okf.conformance('metrics/gross-margin.md', tree);
```

The helpers are specified in [`docs/host-helpers.md`](docs/host-helpers.md)
and pinned by shared fixtures, so the two implementations agree.

## Editor setup

OKF documents are `.md` files, and `okf` is a superset of the markdown that
`tree-sitter-markdown` parses. It is safe to register `okf` for markdown
buffers, ideally only in OKF bundles (spec D11).

### Neovim (nvim-treesitter)

```lua
-- nvim-treesitter `master` branch API
local parsers = require('nvim-treesitter.parsers').get_parser_configs()
parsers.okf = {
  install_info = {
    url = 'https://github.com/antstanley/tree-sitter-okf',
    files = { 'src/parser.c', 'src/scanner.c' },
    branch = 'main',
  },
}
vim.treesitter.language.register('okf', 'markdown')
```

Then run `:TSInstall okf`, and copy `queries/*.scm` into
`~/.config/nvim/queries/okf/`. nvim-treesitter does not fetch queries for
parsers it does not ship. To use `okf` only inside bundles, register it from
an autocommand that checks for the bundle's root `index.md`, instead of
globally.

### Helix

```toml
# languages.toml
[[language]]
name = "okf"
scope = "source.okf"
file-types = ["md"]
roots = ["index.md"]
injection-regex = "okf"

[[grammar]]
name = "okf"
source = { git = "https://github.com/antstanley/tree-sitter-okf", rev = "<commit sha>" }
```

Helix needs a commit SHA for `rev`, so use the commit of a release tag.

Then run `hx --grammar fetch && hx --grammar build`, and copy `queries/*.scm`
into `~/.config/helix/runtime/queries/okf/`.

### Command line

```sh
npx tree-sitter parse path/to/concept.md
npx tree-sitter query queries/okf/fields.scm path/to/concept.md
```

## Development

```sh
npm install                 # tree-sitter CLI and Node bindings
npm run generate            # tree-sitter generate --abi 14
npm test                    # corpus: tree-sitter test
script/test                 # every suite (below); --bench adds the benchmark
```

`script/test` needs a Python environment with `tree-sitter`, `PyYAML`,
`tree-sitter-yaml` and this package installed (`pip install -e .`).

| Suite | What it checks |
|---|---|
| `tree-sitter test` | Hand-reviewed trees in `test/corpus/` (frontmatter, body, documents, errors) |
| `script/diff-upstream` | tree-sitter-markdown's own corpus through this grammar. Every difference is a recorded divergence |
| `script/diff-yaml` | Every frontmatter scalar value against PyYAML |
| `script/test-queries` | Query captures on fixture documents, native and full-YAML modes |
| `script/helpers-fixtures.js` | Host helpers against `test/helpers/cases.json` (Node and Python tests run the same cases) |
| `script/property-test` | No `ERROR` on fuzzed inputs, lossless leaves, incremental == full parse, CRLF/BOM, linear time |
| `script/node-types --check` | `docs/node-types.md` covers every node type |
| `script/verify-vendor` | Vendored markdown sources and the OKF fixture bundles match their SHA-256 locks |
| `script/bench` | Parse throughput and incremental latency against `bench/baseline.json` |

Test fixtures in `test/fixtures/bundles/` are the four official OKF example
bundles, pinned by `LOCK.json` and refreshed with `script/sync-fixtures`.
Upstream markdown is refreshed with `script/revendor` (see `MERGE.md`).

The experimental `[[wiki link]]` and `#tag` dialects are off by default.
Build them with `OKF_DIALECT_WIKILINK=1 OKF_DIALECT_TAGS=1 npm run generate`.

## Repository layout

```
grammar.js               assembles the layers below
grammar/                 block.js, inline.js, common.js (merged markdown), frontmatter.js (OKF-YAML)
src/scanner.c            the external scanner: markdown block + inline, OKF-YAML frontmatter
queries/                 editor queries and the OKF query library
bindings/                C, Go, Node, Python, Rust, Swift (+ host helpers for Node, Python)
docs/                    grammar spec, OKF-YAML subset, node types, host helpers
test/                    corpus, fixture bundles, query and helper fixtures
vendor/tree-sitter-markdown/   pinned upstream sources and MERGE.md
script/                  test, differential, property, bench and maintenance scripts
bench/                   benchmark baseline
```

## License

MIT (see [`LICENSE`](LICENSE)). The markdown rules derive from
tree-sitter-markdown (MIT). The OKF example bundles used as test fixtures
are Apache-2.0. See [`NOTICE`](NOTICE).
