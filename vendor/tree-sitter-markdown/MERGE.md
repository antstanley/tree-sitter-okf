# Merging tree-sitter-markdown into tree-sitter-okf

`tree-sitter-okf` parses markdown with the rules of
[tree-sitter-markdown](https://github.com/tree-sitter-grammars/tree-sitter-markdown).
Its **block** and **inline** grammars are merged into one grammar, so a single
tree (and a single query) covers the frontmatter and every inline of the body
(spec D4(b)).

This directory holds the **unmodified** upstream sources that the merge
started from. They are pinned by `LOCK.json` and checked by
`script/verify-vendor`. They are not compiled. The merged code lives in:

| Upstream | Merged into |
|---|---|
| `tree-sitter-markdown/grammar.js` (`block-grammar.js`) | `grammar/block.js` |
| `tree-sitter-markdown-inline/grammar.js` (`inline-grammar.js`) | `grammar/inline.js` |
| `common/common.js`, `common/html_entities.json` | `grammar/common.js`, `grammar/html_entities.json` |
| `tree-sitter-markdown/src/scanner.c` (`block-scanner.c`) | `src/scanner.c`, block half |
| `tree-sitter-markdown-inline/src/scanner.c` (`inline-scanner.c`) | `src/scanner.c`, inline half |
| `test/corpus` of both grammars (`test/block`, `test/inline`) | Run unchanged by `script/diff-upstream` |

Every intentional difference is listed below. In the sources, each one is
marked `OKF delta <id>` or `OKF:`. Anything else that differs from upstream
is a porting bug.

## Deltas

### C: common rules

| Id | Change | Why |
|---|---|---|
| C1 | The `EXTENSION_*` environment switches are removed. GFM (tables, task lists, strikethrough) is always on, LaTeX and `+++` metadata are always off. `OKF_DIALECT_WIKILINK` / `OKF_DIALECT_TAGS` replace `EXTENSION_WIKI_LINK` / `EXTENSION_TAGS` | OKF pins one profile (spec D4, D7). The dialects are experimental and off by default |
| C2 | `punctuation_without` and `html_entity_regex` are module-level helpers shared by both layers | The two grammars are one now |

### B: block layer

| Id | Change | Why |
|---|---|---|
| B1 | Inline content is parsed in place (`inline` contains the inline rules) instead of being injected. Upstream's shared names `_word`, `_whitespace`, `_line` stay with the inline layer. The block layer's raw-text versions are renamed `_blk_word`, `_blk_whitespace`, `_blk_line` | One grammar (spec D4(b)) |
| B2 | `minus_metadata` / `plus_metadata` are removed. A `---` at byte offset 0 is handed to the OKF-YAML layer (`grammar/frontmatter.js`) | Frontmatter is its own layer (spec D2(c)) |
| B3 | End of input is a zero-width blank line (`STATE_EOF_BLANK_LINE`). A block that needs content (such as `- ` with no final newline) and a fence whose first line ends at EOF close cleanly | Upstream produced `ERROR` for files without a final newline (spec §3.1: a final newline is optional) |
| B4 | The serialized scanner state is canonical. The column is not serialized (the tab width comes from `lexer->get_column`). Delimiter-run state is reset on non-delimiter tokens, inline state on line endings, and indentation after inline tokens | GLR versions that differ only in stale state can now merge. Incremental re-parse dropped from full-parse cost to under 1 ms (spec §9.5) |
| B5 | GFM task list markers `[ ]` / `[x]` are scanner tokens, and the marker includes the whitespace after it | Keeps `[x](url)` a link |
| B6 | Pipe-table rows are single-line content (`STATE_SINGLE_LINE`, so no soft line endings). `\|` is always a cell separator. Cells are `_first_cell` / `_cell_after_pipe` / `_last_cell_after_pipe`, with `_pipe_table_empty_cell`. Every delimiter cell must contain a `-`, and a delimiter row may start with `\|---`. In a row (`STATE_TABLE_ROW`) an unescaped `\|` ends any open code span, emphasis or link, because GFM splits cells before parsing inlines | GFM table semantics. Upstream counted empty cells too, so `\|\|` passed as a delimiter row that the grammar then rejected |
| B7 | The whitespace between an ATX marker and the heading text is a token (`_atx_heading_space`), and ATX headings are single-line | Heading content ranges exclude the space. Headings never absorb a soft line ending |
| B8 | GFM footnote definitions `[^id]: …`, a container block like a list item. The start token is zero width, and consecutive definitions are separate blocks | OKF §5.1 per-claim attribution joins `sources[].id` to footnote labels |
| B9 | A fenced code block's info string may start with `,` | Upstream produced `ERROR` (fuzz finding) |

### I: inline layer

| Id | Change | Why |
|---|---|---|
| I1 | A soft line break is the block scanner's `_soft_line_ending` followed by the next line's `block_continuation` (`> `, list indentation, …) | Upstream's inline grammar saw text with container prefixes already removed |
| I2 | Whitespace and punctuation before a delimiter run are recorded in scanner state (`prev_class`, `prev_column`). Only the characters in `RECORDED_PUNCTUATION` (all ASCII punctuation except `[`) may be emitted by the scanner | Upstream derived left/right flanking from the validity of the `_last_token_*` markers. That information is lost when a subtree is reused incrementally, so emphasis differed between incremental and full parses |
| I3 | A `[` that nothing after it in its paragraph can close is literal text, marked by the zero-width `_literal_open_bracket`. A delimiter run that no closer of its kind follows is literal too (`closer_possible`) | Upstream forked a GLR version on every opener, which is exponential in pathological inputs |
| I4 | The code-span closer lookahead stops at the end of the paragraph, and in single-line content at the end of the line | Upstream looked to the end of its input, which was the paragraph because of the injection. In one grammar the input is the whole document |
| I5 | Footnote references `[^id]`, with the `label` field and a zero-width start | Pairs with B8 |

## Recorded divergences

`test/upstream-divergences.txt` lists the upstream corpus cases whose tree
intentionally differs, with a reason for each. `script/diff-upstream` fails
if a listed case stops differing, or if an unlisted case differs. Current
count: 5 of 651. Two are inline examples that are HTML blocks in a whole
document, two are `#tag` cases (dialect off), and one is a strikethrough
example across a blank line.

## Updating upstream

1. `script/revendor <commit>` replaces this directory's files and rewrites
   `LOCK.json`.
2. `git diff vendor/` shows what upstream changed. Port each change into the
   merged files, keeping the deltas above.
3. `script/test` runs everything. `script/diff-upstream` must show 0
   unexpected differences, and new divergences need an entry and a reason.
4. Update this file if a delta changed, and note the new commit in
   `CHANGELOG.md`.
