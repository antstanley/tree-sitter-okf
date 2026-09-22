# Host helpers

The grammar records **syntax only** (spec P3, D5). Some OKF facts need
something the parse tree does not have: a filename, a clock, the absence of
a key, or what a value means. Those facts are computed by host helpers
(spec §4.4, §7.5).

The helpers are specified here and pinned by one shared fixture file,
`test/helpers/cases.json`, so every binding that ships them behaves the same
way (spec Q10):

| Binding | Module | Naming |
|---|---|---|
| Node (reference) | `require('tree-sitter-okf').okf` (`bindings/node/okf.js`) | camelCase |
| Python | `from tree_sitter_okf import okf` (`bindings/python/tree_sitter_okf/okf.py`) | snake_case |

Other bindings (Rust, Go, Swift, C) ship the grammar and queries. They can
implement the helpers against the same fixtures.

Every helper takes a `Tree` produced by this grammar, or that tree's root
node. The tree must have been parsed from the text being inspected.

```js
const Parser = require('tree-sitter');
const OKF = require('tree-sitter-okf');
const parser = new Parser();
parser.setLanguage(OKF);
const tree = parser.parse(text);
OKF.okf.trustTier(tree);                 // 'human-reviewed'
OKF.okf.conformance('metrics/x.md', tree); // [{rule, severity, message}, ...]
```

```python
import tree_sitter, tree_sitter_okf
from tree_sitter_okf import okf
parser = tree_sitter.Parser(tree_sitter.Language(tree_sitter_okf.language()))
tree = parser.parse(text.encode())
okf.trust_tier(tree)
```

## Reference

### `classify(path, tree)` → `'index' | 'log' | 'concept' | 'non_conformant'`

Spec §4.4, OKF §3.1. Checked in this order:

1. The basename is `index.md`, which gives `index`.
2. The basename is `log.md`, which gives `log`.
3. The tree has a `frontmatter`, which gives `concept`.
4. Otherwise `non_conformant`: a non-index, non-log document with no
   frontmatter.

Backslashes in `path` are treated as separators.

### `conceptId(path, bundleRoot)` / `concept_id`

The path relative to the bundle root, with `/` separators and without the
`.md` suffix. For example, `metrics/gross-margin`.

### `frontmatter(tree)` → `{present, closed, value, unsupported}`

The frontmatter as a plain data value:

* Mappings become objects/dicts and sequences become arrays/lists.
  Tags are ignored. An alias takes the value of an earlier anchor on a
  block node, or `null` if there is none. A merge key `<<` is an ordinary
  key; it is not applied.
* Scalars become **strings**. They are never typed (spec D3). Plain and
  quoted scalars are folded as YAML does: continuation lines are trimmed,
  a line break becomes a space, and each empty line becomes `\n`.
  Double-quoted escapes are decoded; an `\x`, `\u` or `\U` escape that is
  not the right number of hex digits naming a code point (such as the `\U`
  of `"C:\Users"`) is kept as written. Block scalars apply `|`/`>`,
  chomping and the indentation indicator. CRLF line endings are normalised,
  so no value contains the `\r` of a line break.
* An empty value is `null`/`None`.
* Duplicate keys: the last one wins (see `duplicateKeys`).

`present` is false when there is no frontmatter. `closed` is false when the
closing `---` is `MISSING`. `unsupported` is true when any `yaml_unsupported`
node is present. In that case the value is best-effort: unsupported entries
are left out and unsupported values are `null`.

### `fields(tree)`

`frontmatter(tree).value` if it is a mapping, else `{}`.

### `verifiedEntries(tree)` / `verified_entries`

`verified` as a list (OKF §5.2: a bare mapping is a one-element list).
Entries that are not mappings are dropped.

### `trustTier(tree)` / `trust_tier` → `'unverified' | 'human-reviewed' | 'machine-confirmed'`

OKF §5.3. The result is `unverified` if there is no `verified` key,
`human-reviewed` if any verified entry's `by` starts with `human:`, and
`machine-confirmed` otherwise.

### `status(tree)`

OKF §5.4. The `status` value, or `'stable'` when it is absent, empty or
not a string.

### `isStale(tree, now)` / `is_stale` → `boolean | null`

OKF §5.5. Returns `now >= stale_after`. Returns `null` when `stale_after` is
absent or is not an ISO 8601 date or date-time: `YYYY-MM-DD`, optionally
followed by `T`, `t` or a space, `hh:mm[:ss[.fraction]]` and `Z` or an
offset (`±hh:mm` or `±hhmm`), with every field in range (so `2026-02-30`
is `null`). A bare date means midnight UTC, and a date-time without an
offset is UTC, so the answer never depends on the host's time zone. The
fraction counts to the millisecond. `now` defaults to the current time,
and the fixtures pin it.

### `generatedAt(tree)` / `generated_at` → `string | null`

`generated.at`. If `generated` is absent, falls back to the v0.1 `timestamp`
key (OKF §13.1).

### `duplicateKeys(tree)` / `duplicate_keys` → `[{key, count, path}]`

Keys that occur more than once in the same mapping (block or flow), at any
depth. `path` lists the keys and sequence indexes leading to that mapping. The
grammar parses duplicates without complaint (spec §5.4); whether they are an
error is a linter decision.

### `citations(tree)` → `{sources, references, definitions, unresolved, unused}`

Per-claim attribution (OKF §5.1): the join between frontmatter `sources[].id`
and body footnotes. `references` and `definitions` are
`{label, row, column}` for every `footnote_reference` / `footnote_definition`.
`unresolved` lists labels referenced in the body that have no source with
that id. `unused` lists source ids that no footnote references.

### `legacyCitations(tree)` / `legacy_citations` → `string[] | null`

For v0.1 documents: the text of each list item under a heading whose text is
`Citations` (case-insensitive). Returns `null` if there is no such section.

### `conformance(path, tree, {bundleRoot})` → `[{rule, severity, message}]`

Findings for one document (OKF §11). They are advice: OKF forbids rejecting
a bundle for most of them. `bundleRoot` says whether the file is in the
bundle root (Python: `bundle_root=`).

| Rule | Severity | When |
|---|---|---|
| `frontmatter-required` | error | A concept (or unclassifiable) document has no frontmatter |
| `frontmatter-unterminated` | error | The closing `---` is missing |
| `type-required` | error | The frontmatter has no non-empty string `type` |
| `index-frontmatter` | error | `index.md` has frontmatter other than a lone `okf_version` in the bundle root (OKF §8, §12) |
| `log-date-heading` | error | A level-2 heading in `log.md` is not `YYYY-MM-DD` (OKF §9) |
| `frontmatter-outside-subset` | warning | The frontmatter contains `yaml_unsupported` (`docs/okf-yaml.md`) |
| `syntax` | warning | The tree contains an `ERROR` or `MISSING` node (not reported when the frontmatter is unterminated, which is already an error) |

## Fixtures

`test/helpers/cases.json` holds cases that are either a pinned fixture file
from `test/fixtures/bundles` or inline `source` text. Each case also has a
`path`, an optional `now` and `bundleRoot`, and an `expect` object holding
every helper's result.

* `node script/helpers-fixtures.js` checks the Node implementation.
* `--update` regenerates `expect`. Review that diff by hand: it records what
  the code does, not what OKF says.
* `npm run test:bindings` and
  `python -m unittest discover -s bindings/python/tests` run the same cases in
  each binding.

To change a helper's behaviour, change this document, the Node
implementation and every port together, then regenerate and review the
fixtures.
