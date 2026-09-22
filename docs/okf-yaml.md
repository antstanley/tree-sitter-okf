# OKF-YAML — the frontmatter subset

This document is **normative** for the frontmatter layer of `tree-sitter-okf`
(spec §5.1, decision D2(c)). OKF-YAML is a subset of YAML 1.2. It covers
everything the OKF corpus uses, plus the constructs that plausibly appear
around it. It is not YAML, and the grammar never calls it that: the only
promise is that anything **outside** the subset becomes an opaque
`yaml_unsupported` node. **It never becomes `ERROR`.**

The subset is versioned with the parser (see `CHANGELOG.md`). Moving a
construct from out-of-subset into the subset is a minor release, because
`yaml_unsupported` leaves turn into structured nodes. Moving one the other
way is a major release.

Hosts that need full YAML semantics can use the alternative injection in
`queries/injections-fullyaml.scm`. It hands the frontmatter span to
`tree-sitter-yaml`.

## 1. Delimiters

| Rule | Behaviour |
|---|---|
| Opening delimiter | `---` followed by a line ending, at byte offset 0. A leading UTF-8 BOM is skipped. `----`, `--- x`, and a `---` after leading blank lines are **not** delimiters. Those files have no frontmatter (spec Q4: spec-literal). |
| Closing delimiter | `---` at **column 0**. An indented `---` is never a delimiter: it is content of the plain or block scalar it continues (spec Q5). A column-0 `---` always closes, even inside an unclosed quoted scalar or flow collection. YAML 1.2 forbids a document marker there too, and the unfinished value becomes `yaml_unsupported`. Text after the dashes on the closing line is accepted and consumed with the delimiter (lenient: `--- trailing` still closes). |
| Unterminated block | The frontmatter extends to EOF and ends with a `MISSING "---"` node, and the body is absent. A host can then report "missing closing delimiter" instead of "parse error" (spec §4.2, D8). |
| Empty block | `---⏎---⏎` gives a `frontmatter` with no content node. That is valid. The missing `type` is a host-side conformance finding. |
| `...` | The YAML document-end marker at column 0 is an opaque `yaml_document_end` extra. It may appear anywhere a line may start. |

Line endings may be LF or CRLF. `\r` is never part of a scalar.

## 2. In subset

These parse into the structured nodes listed in `docs/node-types.md`.

| # | Construct | Nodes |
|---|---|---|
| 1 | Block mappings `key: value`, nested by indentation | `block_mapping`, `block_mapping_pair` (fields `key`, `value`) |
| 2 | Block sequences `- item`, including **zero-indented** sequences (`key:` followed by `- item` lines at the key's own column) | `block_sequence`, `block_sequence_item` (wraps its value directly) |
| 3 | Compact nesting on one line: `- key: value` (with sibling pairs aligned below), `- - item` | `block_sequence_item` › `block_mapping` / `block_sequence` |
| 4 | Plain scalars, single-line and multi-line (folded continuation lines deeper than the enclosing block). Values may contain `&`, `*`, `#` (not after whitespace), `:` (not before whitespace), `[`, `]`, `{`, `}` and `,` | `plain_scalar` (byte-faithful span, continuation lines included) |
| 5 | Single-quoted scalars with `''` escapes, single- or multi-line | `single_quote_scalar` |
| 6 | Double-quoted scalars with backslash escapes, single- or multi-line (without escaped line breaks, see §3) | `double_quote_scalar` |
| 7 | Quoted keys: `"key": v`, `'key': v` | `block_mapping_pair` with a quoted `key` |
| 8 | Block scalars `\|` and `>`, with chomping (`-`, `+`) and explicit indentation indicators in either order | `block_scalar` (header and body in one leaf) |
| 9 | Flow mappings and sequences, nested to any depth, single- or multi-line, with an optional trailing comma | `flow_mapping`, `flow_sequence`, `flow_pair` (fields `key`, `value`) |
| 10 | Empty values: `key:`, `-`, `key: {}`, `key: []` | pair or item without a value |
| 11 | Anchors `&a`, tags `!t`, `!!t`, `!<verbatim>`, `!` before any node, or standing alone as an empty node's properties | `anchor`, `tag`: children of the pair or item, **never** part of the `value` field |
| 12 | Aliases `*a` as values | `alias` |
| 13 | Merge keys `<<: *base`: recognised as an ordinary key, **not applied** | `block_mapping_pair` |
| 14 | Comments `# …` on their own line or after a node (after whitespace) | `comment` (extra) |
| 15 | Directive lines `%YAML …`, `%TAG …` at column 0 | `yaml_directive` (extra) |
| 16 | Duplicate keys: parsed as ordinary pairs, never an error (spec §5.4) | Flagged by the `duplicateKeys` host helper |

Scalars are **never typed** (spec D3). `true`, `2026-07-01` and `3.5` are all
`plain_scalar` spans. Typing policy lives in `queries/okf/scalars.scm`, and
hosts may replace it.

## 3. Out of subset → `yaml_unsupported`

Each of these becomes one `yaml_unsupported` leaf, usually covering the rest
of its line (`yaml_unsupported` spans the first opaque token and the rest of
the line). The node sits where the construct was, and the lines around it
still parse normally.

| Construct | Example | Where the leaf goes |
|---|---|---|
| Explicit (complex) keys | `? key` / `: value` | Each line is a leaf |
| Alias or collection as a key | `*a: b`, `[a, b]: c`, `{a: b}: c` | The whole line |
| A second `: ` on a line | `a: b: c` | The pair's `value` |
| Trailing content after a quoted scalar or flow collection | `a: "x" y`, `a: [b] c` | The pair's `value` |
| Unbalanced flow collections | `k: [unclosed` | The pair's `value` |
| Double-quoted scalars with an escaped line break | `"esc\⏎  cont"` | The pair's `value` |
| Reserved indicators starting a scalar | `` a: `x ``, `a: @x` | The pair's `value` |
| A line starting with `:` | `: v` | The line |
| A line deeper than its block that nothing can own | `a: b⏎  c: d` | The over-indented line |
| A line shallower than its block but deeper than the parent | `a:⏎  - b⏎ c: d` | The line |
| A line shallower than the root, when the first line was indented | `  a: 1⏎b: 2` | The line |
| A sibling of a different kind | `- a⏎b: c` (a mapping pair after a root sequence) | The line |
| An indented `%` line (a directive is only at column 0) | `a:⏎  %x` | The pair's `value` |
| Nesting deeper than 48 levels | — | Each line past the limit |
| Anything else YAML 1.2 allows that §2 does not list | — | — |

A `yaml_unsupported` node is **grep-able**: CI or a linter can report
`frontmatter-outside-subset` (the `conformance` host helper does this), so a
producer finds out without the parse failing.

## 4. Indentation

* Indentation is counted in columns, and spaces count 1.
* **Tabs** advance to the next multiple of **8** (spec Q6). YAML forbids tab
  indentation. OKF-YAML accepts it with this fixed width so an editor never
  sees a broken tree, and a linter can flag the tab. Columns later on a line
  (such as the key in `⇥- key: v`) are measured the same way, so a sibling
  line indented with the same tabs lines up.
* A node after anchors or tags (`&a key: v`) stands at the column of the
  first property, for indentation purposes.
* The first content line fixes the root indentation, which does not have to
  be column 0.
* Blank lines, comment lines and directive lines never affect structure.

## 5. Differences from YAML 1.2 worth knowing

* **Lenient closer.** `--- trailing text` closes the frontmatter. In YAML it
  would start a new document.
* **Tab indentation** is accepted (width 8). YAML rejects it.
* **No typing, no anchors resolved, no merge applied.** See D3 and Q3. The
  tree is a faithful syntax tree, not a data model.
* **Multiple documents** do not exist. The first column-0 `---` closes the
  frontmatter and the body starts after it.

The differential suite (`script/diff-yaml`) checks every structured scalar
in the fixtures and corpus against PyYAML's `BaseLoader`. Blocks that
contain `yaml_unsupported`, or that the reference parser rejects, are
skipped and counted.
