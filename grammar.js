/**
 * @file Open Knowledge Format (OKF) grammar for tree-sitter
 * @license MIT
 *
 * OKF v0.2 documents are markdown with YAML frontmatter, organised into
 * knowledge bundles.  This grammar parses ONE document (an OKF concept
 * document, an `index.md` or a `log.md`) into one tree spanning both the
 * frontmatter and the body.  The design is docs/grammar-spec.md; this file only
 * assembles the three layers:
 *
 *   grammar/frontmatter.js   OKF-YAML, the frontmatter subset (spec §5)
 *   grammar/block.js         markdown block structure (spec §6.1)
 *   grammar/inline.js        markdown inlines (spec §6.2)
 *   grammar/common.js        rules both markdown layers share
 *
 * The markdown layers are tree-sitter-markdown's block and inline grammars
 * merged into one, with the GFM profile pinned on and footnotes added (spec
 * D4(b)); every local change is listed in MERGE.md.  All external tokens live
 * in one scanner, `src/scanner.c`, whose token enum MUST list the externals in
 * exactly the order this file does.
 *
 * Two principles drive everything (spec §2.1):
 *
 *   P3  A node exists iff its presence is decidable from THIS file's syntax.
 *       Nothing depends on the filename, the bundle, a clock or another file.
 *
 *   P4  Plausible input never produces ERROR.  Constructs outside the profile
 *       become opaque nodes; ERROR is reserved for genuinely unsalvageable
 *       bytes.
 */

/// <reference types="tree-sitter-cli/dsl" />

const block = require('./grammar/block');
const inline = require('./grammar/inline');
const frontmatter = require('./grammar/frontmatter');

module.exports = grammar({
  name: 'okf',

  // Order is load-bearing: see the TokenType enum in src/scanner.c.
  externals: ($) => [
    ...block.externals($),
    ...inline.externals($),
    ...frontmatter.externals($),
  ],

  // Whitespace is not an extra: it is significant in both layers (YAML
  // indentation, markdown indentation and hard line breaks).  The extras below
  // are frontmatter-only scanner tokens that the scanner never emits in the
  // body.
  extras: ($) => frontmatter.extras($),

  precedences: ($) => [
    ...block.precedences($),
    ...inline.precedences($),
  ],

  conflicts: ($) => [
    ...block.conflicts($),
    ...inline.conflicts($),
    ...frontmatter.conflicts($),
    // Conflicts that exist only because the block and inline grammars are
    // merged: a block-level construct (a link reference definition, a table
    // row) and a paragraph's inline content can start with the same tokens.
    // They are resolved by GLR, exactly as upstream resolves the same
    // ambiguity across its two grammars.  Reviewed list; see MERGE.md.
    ...require('./grammar/merge-conflicts.json').map((names) => names.map((n) => $[n])),
  ],

  rules: {
    /**
     * The OKF document shape, and nothing more (spec §3.2):
     *
     *     source_file = [ frontmatter ] , body
     *
     * There is deliberately no `index_document` / `log_document`: telling them
     * apart needs the filename, which a grammar does not have (spec D6).  See
     * the `classify(path, tree)` helper in the bindings.
     *
     * `body` is always present, possibly zero-width, except when the
     * frontmatter is unterminated: then the closing `---` is MISSING and the
     * rest of the file is frontmatter (spec §4.2).
     */
    source_file: ($) => seq(
      optional(field('frontmatter', $.frontmatter)),
      optional(field('body', $.body)),
    ),

    /**
     * The markdown body.  Top-level blocks before the first heading, then one
     * `section` per top-level heading, as in tree-sitter-markdown (spec Q8).
     * The zero-width end-of-file token is what lets an empty body still be a
     * node.
     */
    body: ($) => seq(
      prec.right(repeat($._block_not_section)),
      repeat($.section),
      $._eof,
    ),

    /** Inline content: a paragraph's text, a heading's text, a table cell. */
    inline: ($) => seq(optional($._last_token_whitespace), $._inline),

    ...frontmatter.rules,
    ...block.rules,
    ...inline.rules,
  },
});
