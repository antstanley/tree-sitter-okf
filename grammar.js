/**
 * @file Open Knowledge Format (OKF) grammar for tree-sitter
 * @license MIT
 *
 * OKF v0.2 is markdown documents with YAML frontmatter, organised into
 * knowledge bundles.  This grammar parses ONE document: an OKF concept
 * document, an `index.md`, or a `log.md`.  See docs/grammar-spec.md for the
 * design and docs/okf-yaml.md for the frontmatter subset this accepts.
 *
 * Layout:
 *
 *   1. helpers            small DSL conveniences
 *   2. externals          both scanners' token sets
 *   3. document level     source_file / frontmatter / body
 *   4. OKF-YAML           the frontmatter layer
 *   5. markdown blocks    the OKF markdown profile, block layer
 *   6. markdown inlines   the OKF markdown profile, inline layer
 *
 * Two principles from the spec drive everything here:
 *
 *   P3  A node exists iff its presence is decidable from THIS file's syntax.
 *       Nothing needs the filename, the bundle, a clock, or another file.
 *
 *   P4  Plausible input never produces ERROR.  Constructs outside the profile
 *       become opaque nodes; ERROR is reserved for genuinely unsalvageable
 *       bytes.
 *
 * ## How the two scanners divide the work
 *
 * `src/scanner.c` implements an OKF-YAML scanner and a markdown scanner.  They
 * are told apart by which symbols are valid in the current parse state, and
 * their regions are disjoint, so they share no state.
 *
 * The markdown scanner is almost entirely *stateless local lookahead*.  The one
 * decision it has to make that the parser cannot — "is the line after this one
 * ending a continuation, or a new block?" — it makes by looking at that line.
 * Lookahead is safe because tree-sitter's `mark_end` both ends the token and
 * sets where the next token starts, so anything peeked past the mark is
 * re-lexed.  External tokens also take precedence over longer internal matches,
 * which is what lets a heading marker claim `# ` before inline `text` can
 * swallow the line.
 *
 * The frontmatter scanner does keep state (an indentation stack), because YAML
 * block structure cannot be decided locally.  That state is confined behind the
 * region boundary, where the frontmatter grammar is deterministic, and it is
 * covered by the "incremental equals full parse" property test.
 */

/// <reference types="tree-sitter-cli/dsl" />

/* ------------------------------------------------------------------ 1. helpers */

/**
 * How deep frontmatter block nesting may go before it is flattened.  The sample
 * bundles reach two levels.  Beyond the last level, deeper lines are treated as
 * siblings, which mis-nests pathological input but never errors (P4).
 */
const FM_LEVELS = 4;

/** Fence content, per fence character.  A content line must not be a closing
 *  fence, which needs no scanner state: closing fences win the lexical contest
 *  on their own, and a fence whose closing marks are missing runs to EOF. */
const fencePattern = (ch) =>
  ` *(?:${ch}{0,2}(?:[^${ch}\\n][^\\n]*)?|[^${ch}\\n][^\\n]*)`;

const fenceContent = (ch) => choice(
  token(prec(-1, new RegExp(fencePattern(ch) + '\\r?\\n'))),
  // the same line without an ending, so a fence that runs to end of input
  // without a final newline still parses
  token(prec(-2, new RegExp(fencePattern(ch)))),
);

/** A GFM pipe-table delimiter row: `| --- | :--: |`. */
const tableDelimiterRow = / *\|?[ \t]*:?-+:?[ \t]*(?:\|[ \t]*:?-+:?[ \t]*)*\|?[ \t]*/;

/** Literal characters that also introduce other constructs.  They are separate
 *  nodes so the tree records *why* a character was not part of a link, a fence
 *  or an entity — which is what a producer needs when a link fails to parse. */
const literals = {
  pipe_literal: (_) => '|',
  bang_literal: ($) => $._bang_literal,
  paren_literal: (_) => choice('(', ')'),
  angle_literal: (_) => '<',
  ampersand_literal: (_) => '&',
  tilde_literal: (_) => '~',
};

module.exports = grammar({
  name: 'okf',

  externals: ($) => [
    /* --- document level -------------------------------------------------- */
    $._bom, // a leading U+FEFF
    $._eof, // zero width, valid only at end of input

    /* --- OKF-YAML scanner -------------------------------------------------
     * Entry tokens carry an absolute block level so the grammar can nest
     * without any dedent markers: a token for level 2 simply cannot appear
     * inside a level-1 block.  An entry token spans the line ending, the blank
     * and comment lines before it, its indentation and its key text (up to and
     * including the `:` for a key).
     */
    $._fm_open, // '---' + rest of line at byte 0 (BOM tolerant)
    $._fm_close, // '---' at column 0, never inside a scalar
    $._fm_sep, // the line ending and blanks before the closing delimiter
    // Entry tokens come in two flavours.  `_fm_key_lN` means a value follows on
    // the same line; `_fm_key_e_lN` means the value is empty, which is the only
    // case in which a nested block may follow.  Splitting them this way removes
    // the ambiguity an `optional(value)` would leave behind.
    $._fm_key_l0, $._fm_key_l1, $._fm_key_l2, $._fm_key_l3,
    $._fm_dash_l0, $._fm_dash_l1, $._fm_dash_l2, $._fm_dash_l3,
    $._fm_key_e_l0, $._fm_key_e_l1, $._fm_key_e_l2, $._fm_key_e_l3,
    $._fm_dash_e_l0, $._fm_dash_e_l1, $._fm_dash_e_l2, $._fm_dash_e_l3,
    $._fm_plain, // plain scalar, including multi-line folding
    $._fm_sq, // single quoted scalar ('' escaping)
    $._fm_dq, // double quoted scalar, possibly spanning lines
    $._fm_block_scalar, // '|' / '>' plus its indented body
    $._fm_annotation, // anchor, alias or tag; opaque
    $._fm_comment, // '# ...'
    $._fm_unsupported, // outside the subset; opaque, never fatal
    // Flow punctuation is scanned too, so the scanner can keep track of
    // whether it is inside a flow collection (where indentation is inert).
    $._fm_lbrace, $._fm_rbrace, $._fm_lbracket, $._fm_rbracket,
    $._fm_comma, $._fm_colon,

    /* --- markdown scanner -------------------------------------------------
     * Line breaks are explicit: `_soft_break` is a line ending that is a
     * continuation of the current paragraph, `_line_end` is any other one.
     * Deciding between them needs one line of lookahead, which the scanner does
     * by committing to the line ending and peeking past it.
     */
    $._soft_break,
    $._line_end,

    /* Line-start markers.  All of these would lose a longest-match contest
     * against inline `text`, which is why they live in the scanner. */
    $._atx_h1_marker,
    $._atx_h2_marker,
    $._atx_h3_marker,
    $._atx_h4_marker,
    $._atx_h5_marker,
    $._atx_h6_marker,
    $._setext_h1_underline,
    $._setext_h2_underline,
    $._thematic_break,
    $._block_quote_marker,
    $._list_marker_star,
    $._list_marker_minus,
    $._list_marker_plus,
    $._list_marker_ordered,
    $._fence_open_backtick,
    $._fence_open_tilde,
    $._fence_close_backtick,
    $._fence_close_tilde,
    $._table_pipe, // '|' as a cell separator, only valid inside a table row
    $._image_start, // '!' when it introduces an image
    $._bang_literal, // '!' when it does not
    // Brackets are scanned too.  Whether a `[` opens a link, opens a footnote
    // reference, or is just a bracket is a two-character question, and the
    // answer has to come from somewhere that can look ahead.
    $._link_open, // '[' that begins a `[...](...)` construct
    $._link_close, // its matching ']'
    $._footnote_reference, // '[^label]', one token
    $._footnote_definition_label, // '[^label]:' at the start of a line
    $._bracket_literal, // a '[' or ']' that opens nothing
    $._indented_code_line,

    /* Inline delimiters.  A run of backticks or of `*`/`_` is matched in the
     * scanner so that a run with no closer can stay literal text instead of
     * derailing the parse (P4). */
    $._code_span_open,
    $._code_span_content,
    $._code_span_close,
    $._emphasis_open,
    $._emphasis_close,
    $._strong_open,
    $._strong_close,
  ],

  /**
   * Whitespace is NOT an extra.
   *
   * In frontmatter it is significant because block structure is carried by
   * indentation; in the body it is significant because fences and indented code
   * depend on it.  Both layers therefore consume their own whitespace, which is
   * also why this list is empty.
   */
  extras: ($) => [],

  /**
   * Table cells and inline runs share almost their whole element set, and a
   * cell's inline content can itself contain an inline run (link text), so the
   * parser has to fork at a cell boundary.  The fork always resolves: the
   * `_table_pipe` scanner token delimits cells, so only one reading survives.
   */
  conflicts: ($) => [
    // A table cell's inline content can itself contain an inline run, so the
    // parser forks at a cell boundary.  The fork always resolves: `_table_pipe`
    // delimits cells.
    [$.table_cell, $.inline],
    // Consecutive list items, and consecutive quoted lines, can each be read as
    // one construct or as two adjacent ones.  The readings mean the same thing,
    // so either is fine.
    [$.list],
    [$.block_quote],
    [$.paragraph, $.inline],
    // Every block that may be followed by a blank line can be read as ending
    // there or as continuing; both readings are equivalent.
    [$.footnote_definition],
    // A trailing frontmatter comment can be read as belonging to the entry it
    // follows or as standing alone.  Only the first reading is ever viable;
    // declaring the conflict lets the parser find that out.
    [$.block_mapping],
    [$.block_mapping_l1],
    [$.block_mapping_l2],
    [$.block_mapping_l3],
    [$.block_sequence],
    [$.block_sequence_l1],
    [$.block_sequence_l2],
    [$.block_sequence_l3],
    [$.block_mapping_pair],
    [$.block_mapping_pair_l1],
    [$.block_mapping_pair_l2],
    [$.block_mapping_pair_l3],
    [$.block_mapping_pair_empty],
    [$.block_mapping_pair_empty_l1],
    [$.block_mapping_pair_empty_l2],
    [$.block_mapping_pair_empty_l3],
    [$.block_sequence_item],
    [$.block_sequence_item_l1],
    [$.block_sequence_item_l2],
    [$.block_sequence_item_l3],
    [$.block_sequence_item_empty],
    [$.block_sequence_item_empty_l1],
    [$.block_sequence_item_empty_l2],
    [$.block_sequence_item_empty_l3],
    [$.flow_mapping_entries],
    [$.flow_mapping_entry],
    [$.flow_sequence_entries],
    [$.indented_code_block],
    [$.list_item],
    [$.fenced_code_block],
    [$.pipe_table],
    // A heading's content is an optional inline run, and the section that
    // follows starts with blocks that may also begin with inline content.  The
    // fork resolves on the line ending.
    [$._section1],
    [$._section2],
    [$._section3],
    [$._section4],
    [$._section5],
    [$._section6],
    [$._heading1, $.inline],
    [$._heading2, $.inline],
    [$._heading3, $.inline],
    [$._heading4, $.inline],
    [$._heading5, $.inline],
    [$._heading6, $.inline],
  ],

  supertypes: ($) => [
    $._fm_node,
    $._fm_block,
    $._fm_value,
    $._flow_node,
    $._inline_element,
    $._table_inline_element,
    $._block_not_section,
  ],

  inline: ($) => [
    $._fm_value,
    $._flow_node,
    $._block_not_section,
    $._inline_element,
    $._table_inline_element,
    $._fence_close,
    $._fm_entry_l0,
    $._fm_entry_l1,
    $._fm_entry_l2,
    $._fm_entry_l3,
  ],

  rules: {
    /* ===================================================== 3. document level */

    /**
     * The OKF document shape, and nothing more:
     *
     *     source_file = [ frontmatter ] , body
     *
     * There is deliberately no `index_document` / `log_document`.  Telling
     * those apart needs the filename, which a grammar does not have (P3, spec
     * D6).  See README.md for the `classify(path, tree)` helper bindings ship.
     */
    source_file: ($) => seq(
      optional($._bom),
      optional(field('frontmatter', $.frontmatter)),
      optional(field('body', $.body)),
    ),

    /**
     * The frontmatter block.  Both delimiters are scanner tokens: the opener is
     * recognised only at byte offset 0 (BOM tolerant), the closer only at
     * column 0 and never while the scanner is inside a scalar (spec §4.2, Q5).
     *
     * An unterminated block gets a MISSING closer rather than failing, so a
     * host can report "add a closing ---" instead of "parse error" (spec D8).
     */
    frontmatter: ($) => seq(
      field('open', $._fm_open),
      optional(field('content', $._fm_node)),
      optional($._fm_sep),
      field('close', $._fm_close),
    ),

    /* ==================================================== 4. OKF-YAML layer */

    /**
     * The subset documented in docs/okf-yaml.md.  Nothing here resolves YAML
     * scalar *types*: `plain_scalar` is a byte-faithful run and typing is a
     * query-level policy (spec D3, queries/okf/scalars.scm).
     */
    _fm_node: ($) => choice(
      $._fm_block_0,
      $._fm_scalar,
      $.flow_mapping,
      $.flow_sequence,
      $.block_scalar,
      $.comment,
      $.yaml_unsupported,
    ),

    _fm_block: ($) => choice($._fm_block_0, $._fm_block_1, $._fm_block_2, $._fm_block_3),

    /**
     * An inline-ish value.  Nested blocks are *not* listed here: each block
     * level adds exactly the next level down (see the generated rules), which
     * is what stops a sibling entry from being mistaken for a nested block.
     */
    _fm_value: ($) => choice(
      $._fm_scalar,
      $.flow_mapping,
      $.flow_sequence,
      $.block_scalar,
    ),

    ...Object.fromEntries([0, 1, 2, 3].flatMap((n) => {
      const key = `_fm_key_l${n}`;
      const dash = `_fm_dash_l${n}`;
      const keyE = `_fm_key_e_l${n}`;
      const dashE = `_fm_dash_e_l${n}`;
      const mapping = n === 0 ? 'block_mapping' : `block_mapping_l${n}`;
      const sequence = n === 0 ? 'block_sequence' : `block_sequence_l${n}`;
      const item = n === 0 ? 'block_sequence_item' : `block_sequence_item_l${n}`;
      const itemE = n === 0 ? 'block_sequence_item_empty' : `block_sequence_item_empty_l${n}`;
      const pair = n === 0 ? 'block_mapping_pair' : `block_mapping_pair_l${n}`;
      const pairE = n === 0 ? 'block_mapping_pair_empty' : `block_mapping_pair_empty_l${n}`;
      // A nested block can only live one level down from where it starts, and
      // `$` is only in scope inside a rule function, so the choice is built
      // there.
      const deeper = (g) => n + 1 < FM_LEVELS
        ? [g[`block_mapping_l${n + 1}`], g[`block_sequence_l${n + 1}`]]
        : [];
      const entry = (g) => choice(g[key], g[dash], g[keyE], g[dashE]);
      return [
        [`_fm_block_${n}`, ($) => choice($[mapping], $[sequence])],
        [`_fm_entry_l${n}`, ($) => entry($)],
        [mapping, ($) => seq(
          choice($[pair], $[pairE]),
          repeat(choice($[pair], $[pairE], $.yaml_unsupported)),
        )],
        [pair, ($) => seq(
          field('key', alias($[key], $.yaml_key)),
          optional($._fm_spaces),
          field('value', $._fm_value),
          optional(field('comment', $.comment)),
        )],
        [pairE, ($) => seq(
          field('key', alias($[keyE], $.yaml_key)),
          optional(field('value', choice(...deeper($)))),
          optional(field('comment', $.comment)),
        )],
        [sequence, ($) => seq(
          choice($[item], $[itemE]),
          repeat(choice($[item], $[itemE], $.yaml_unsupported)),
        )],
        [item, ($) => seq(
          field('dash', alias($[dash], $.sequence_dash)),
          optional($._fm_spaces),
          field('value', $._fm_value),
          optional(field('comment', $.comment)),
        )],
        [itemE, ($) => seq(
          field('dash', alias($[dashE], $.sequence_dash)),
          optional(field('value', choice(...deeper($)))),
          optional(field('comment', $.comment)),
        )],
      ];
    })),

    /**
     * A quoted key, which is legal YAML.  Plain keys are the scanner's `_fm_key_*`
     * tokens, which include their `:`; this rule exists so a producer who writes
     * `"type": X` still gets a key node rather than an opaque one.
     */
    quoted_key: ($) => seq(
      choice($.single_quote_scalar, $.double_quote_scalar),
      optional($._fm_spaces),
      $._fm_colon,
    ),

    /* --- flow collections ------------------------------------------------- */

    flow_mapping: ($) => seq(
      $._fm_lbrace,
      optional(seq(optional($._fm_spaces), $.flow_mapping_entries)),
      optional($._fm_spaces),
      $._fm_rbrace,
    ),

    /** A trailing comma is legal YAML, so the tail entry is optional. */
    flow_mapping_entries: ($) => seq(
      $.flow_mapping_entry,
      repeat(seq(
        optional($._fm_spaces),
        $._fm_comma,
        optional($._fm_spaces),
        optional($.flow_mapping_entry),
      )),
    ),

    flow_mapping_entry: ($) => seq(
      field('key', $._flow_key),
      optional($._fm_spaces),
      $._fm_colon,
      optional(seq(optional($._fm_spaces), field('value', $._flow_node))),
    ),

    /** Flow keys are ordinary scalar nodes, so a query can read them the same
     *  way it reads block keys. */
    _flow_key: ($) => choice($.plain_scalar, $.single_quote_scalar, $.double_quote_scalar),

    flow_sequence: ($) => seq(
      $._fm_lbracket,
      optional(seq(optional($._fm_spaces), $.flow_sequence_entries)),
      optional($._fm_spaces),
      $._fm_rbracket,
    ),

    flow_sequence_entries: ($) => seq(
      $._flow_node,
      repeat(seq(
        optional($._fm_spaces),
        $._fm_comma,
        optional($._fm_spaces),
        optional($._flow_node),
      )),
    ),

    _flow_node: ($) => choice(
      $._fm_scalar,
      $.flow_mapping,
      $.flow_sequence,
    ),

    /* --- scalars ---------------------------------------------------------- */

    _fm_scalar: ($) => choice(
      $.plain_scalar,
      $.single_quote_scalar,
      $.double_quote_scalar,
      $.annotation,
    ),

    plain_scalar: ($) => $._fm_plain,
    single_quote_scalar: ($) => $._fm_sq,
    double_quote_scalar: ($) => $._fm_dq,
    block_scalar: ($) => $._fm_block_scalar,

    /**
     * Anchors, aliases and tags are recognised but never interpreted (spec D2,
     * Q3).  Opaque means we can never be wrong about them, only incomplete.
     */
    annotation: ($) => $._fm_annotation,

    comment: ($) => $._fm_comment,

    /**
     * The never-fail escape hatch.  Anything the subset does not cover becomes
     * one opaque leaf, so damage stays local instead of becoming an ERROR
     * (spec D8, P4).
     */
    yaml_unsupported: ($) => seq($._fm_unsupported),

    _fm_spaces: (_) => /[ \t]+/,

    /* ============================================ 5. markdown profile, blocks */

    /**
     * The body.  A document with frontmatter and no body has no `body` node at
     * all, because a tree-sitter rule may not match the empty string; queries
     * that want "anything after the frontmatter" should use `source_file`.
     */
    body: ($) => repeat1(choice($._block, $.blank_line)),

    _block: ($) => choice($.section, $._block_not_section),

    /**
     * A section groups a heading with everything up to the next heading of the
     * same or higher level.  This mirrors `tree-sitter-markdown`, so "the code
     * fence under `# Computation`" is a containment query and editors can fold
     * and outline (spec Q8).
     */
    section: ($) => choice(
      $._section1, $._section2, $._section3,
      $._section4, $._section5, $._section6,
    ),

    _block_not_section: ($) => choice(
      $.paragraph,
      $.fenced_code_block,
      $.indented_code_block,
      $.block_quote,
      $.list,
      $.thematic_break,
      $.pipe_table,
      $.footnote_definition,
    ),

    /* --- headings --------------------------------------------------------- */

    atx_heading_marker: ($) => choice(
      $._atx_h1_marker, $._atx_h2_marker, $._atx_h3_marker,
      $._atx_h4_marker, $._atx_h5_marker, $._atx_h6_marker,
    ),

    setext_heading: ($) => seq(
      field('content', $.inline),
      $._line_end,
      field('marker', $.setext_underline),
      $._line_end,
    ),

    setext_underline: ($) => choice($._setext_h1_underline, $._setext_h2_underline),

    /* --- paragraphs ------------------------------------------------------- */

    /**
     * A paragraph is a run of inline elements joined by soft line breaks.  Which
     * line endings are soft is the scanner's decision, made by looking at the
     * line that follows: `# Schema` starts a heading rather than continuing the
     * prose above it, while an ordinary wrapped line continues it.
     */
    paragraph: ($) => seq(
      $.inline,
      repeat(seq($._soft_break, $.inline)),
      $._line_end,
    ),

    /* --- code ------------------------------------------------------------- */

    /**
     * Indented code matters: OKF's own Attested Computation examples put the
     * inline computation in a four-space block (OKF §10.2, §10.3).
     */
    indented_code_block: ($) => seq(
      $._indented_code_line,
      $._line_end,
      repeat(choice(seq($._indented_code_line, $._line_end), $.blank_line)),
    ),

    fenced_code_block: ($) => seq(
      field('fence', choice($._fence_open_backtick, $._fence_open_tilde)),
      optional(field('info', $.info_string)),
      $._line_end,
      optional(field('content', $.code_fence_content)),
      optional(seq(field('close', $._fence_close), $._line_end)),
    ),

    _fence_close: ($) => choice($._fence_close_backtick, $._fence_close_tilde),

    /**
     * Fence content is one token per line rather than one token per block: it
     * keeps the tree line-addressable, which editors and diagnostics want, and
     * keeps the two fence characters unambiguous without scanner state.
     */
    code_fence_content: ($) => repeat1(choice(fenceContent('`'), fenceContent('~'))),

    info_string: ($) => seq(
      field('language', $.language),
      optional(seq($._spaces, optional(field('rest', $.info_string_text)))),
    ),

    language: (_) => /[^ \t\n`~]+/,
    info_string_text: (_) => /[^\n]+/,

    /* --- other leaf blocks ------------------------------------------------ */

    thematic_break: ($) => seq($._thematic_break, $._line_end),

    blank_line: (_) => token(prec(1, /[ \t]*\r?\n/)),

    /**
     * Block quotes are modelled line by line, so `> > text` parses (the inner
     * `>` is literal text) rather than nesting into two nodes.  A documented
     * deviation: no block quote appears in the sample bundles.
     */
    block_quote: ($) => seq(
      $._block_quote_marker,
      optional(field('content', $.inline)),
      $._line_end,
      repeat(seq($._block_quote_marker, optional($.inline), $._line_end)),
    ),

    /* --- lists ------------------------------------------------------------ */

    list: ($) => repeat1($.list_item),

    /**
     * A list item is a marker, an inline first line, then any mix of
     * continuation lines, blank lines and nested blocks.  Whether a following
     * line continues the item is decided by the same one-line lookahead that
     * decides soft line breaks, so no list indentation state is kept.
     */
    list_item: ($) => seq(
      field('marker', $.list_marker),
      optional(field('content', $.inline)),
      $._line_end,
      repeat(choice(
        seq($._soft_break, optional($.inline)),
        $._line_end,
        $.blank_line,
        $.fenced_code_block,
        $.indented_code_block,
        $.pipe_table,
        $.footnote_definition,
      )),
    ),

    list_marker: ($) => choice(
      $._list_marker_star,
      $._list_marker_minus,
      $._list_marker_plus,
      $._list_marker_ordered,
    ),

    /* --- tables ----------------------------------------------------------- */

    /**
     * A table is recognised only when the line after the header row is a
     * delimiter row.  That check is in the scanner (`_pipe_table_start`), so a
     * lone `| x |` stays a paragraph instead of becoming a half-parsed table.
     */
    /**
     * A table.  The delimiter row is *optional* in the grammar even though GFM
     * requires it: deciding whether it is there needs a two-line lookahead the
     * scanner cannot perform without committing to what it peeked, and a
     * required delimiter row would turn a lone `| x |` line into an ERROR.
     * A one-row table is the cheaper mistake (spec P4).
     */
    pipe_table: ($) => seq(
      field('header', alias($.pipe_table_row, $.pipe_table_header)),
      $._line_end,
      optional(seq(
        field('delimiter', $.pipe_table_delimiter_row),
        $._line_end,
        repeat(seq(field('row', $.pipe_table_row), $._line_end)),
      )),
    ),

    pipe_table_delimiter_row: (_) => token(prec(1, tableDelimiterRow)),

    /** GFM allows a row to omit its leading or trailing pipe. */
    pipe_table_row: ($) => seq(
      optional(field('cell', $.table_cell)),
      repeat1(seq($._table_pipe, optional($.table_cell))),
    ),

    table_cell: ($) => prec.right(repeat1($._table_inline_element)),

    /* --- attribution ----------------------------------------------------- */

    /**
     * GFM footnote definitions.  The `sources[].id` keys in frontmatter join to
     * these labels, which is the single most OKF-specific relation in the format
     * (OKF §5.1) and the reason the body lives in the same grammar as the
     * frontmatter.
     */
    footnote_definition: ($) => seq(
      field('label', alias($._footnote_definition_label, $.footnote_label)),
      optional(field('content', $.inline)),
      $._line_end,
      repeat(choice(
        seq($._soft_break, optional($.inline)),
        $._line_end,
        $.blank_line,
        $.fenced_code_block,
        $.indented_code_block,
      )),
    ),

    /* =========================================== 6. markdown profile, inlines */

    /**
     * An inline run.  Right associativity is what lets an inline run nest
     * inside another (link text, image descriptions) without the parser having
     * to guess which one an element belongs to: the outer run extends.
     */
    inline: ($) => prec.right(repeat1($._inline_element)),

    _inline_element: ($) => choice(
      $.code_span,
      $.inline_link,
      $.image,
      $.footnote_reference,
      $.autolink,
      $.emphasis,
      $.strong_emphasis,
      $.backslash_escape,
      $.entity_reference,
      $.html_tag,
      $.pipe_literal,
      $.bang_literal,
      $.paren_literal,
      $.angle_literal,
      $.ampersand_literal,
      $.tilde_literal,
      $.bracket_literal,
      $.text,
    ),

    /** Inside a table cell `|` is a separator, never literal text. */
    _table_inline_element: ($) => choice(
      $.code_span,
      $.inline_link,
      $.image,
      $.footnote_reference,
      $.autolink,
      $.emphasis,
      $.strong_emphasis,
      $.backslash_escape,
      $.entity_reference,
      $.html_tag,
      $.bang_literal,
      $.paren_literal,
      $.angle_literal,
      $.ampersand_literal,
      $.tilde_literal,
      $.bracket_literal,
      $.text,
    ),

    ...literals,

    /**
     * The workhorse: a run of plain prose.  It excludes the characters that
     * introduce other constructs, so those get a chance to be parsed and, when
     * they do not form anything, become one of the literal nodes above.  That
     * is what keeps a stray `[` or `<` from failing the whole line.
     */
    text: (_) => token(prec(-1, /[^\[\]()\\&<!\n|~]+/)),

    code_span: ($) => seq(
      field('open', $._code_span_open),
      optional(field('content', alias($._code_span_content, $.code_span_content))),
      field('close', $._code_span_close),
    ),

    inline_link: ($) => seq(
      field('open', $._link_open),
      optional(field('text', alias($.inline, $.link_text))),
      field('close', $._link_close),
      '(',
      optional(seq(
        optional(field('destination', $.link_destination)),
        optional(seq($._spaces, field('title', $.link_title))),
      )),
      ')',
    ),

    /**
     * A destination is any run that cannot end the link: whitespace and `)`
     * terminate it.  Classifying it as external / bundle-relative / relative /
     * fragment is a query, not a node (spec §7.4), because "path-shaped" is not
     * the same as "resolvable" — `sources[].resource` may be a scope descriptor
     * such as `all queries in BigQuery project X`.
     */
    link_destination: (_) => token(prec(1, /[^\s)]+/)),

    link_title: (_) => choice(
      /"[^"\n]*"/,
      /'[^'\n]*'/,
      /\([^)\n]*\)/,
    ),

    _spaces: (_) => /[ \t]+/,

    image: ($) => seq(
      $._image_start,
      field('open', $._link_open),
      optional(field('description', alias($.inline, $.image_description))),
      field('close', $._link_close),
      '(',
      optional(seq(
        optional(field('destination', $.link_destination)),
        optional(seq($._spaces, field('title', $.link_title))),
      )),
      ')',
    ),

    /**
     * `[^label]`.  The whole reference is one scanner token, because deciding
     * whether a `[` starts a footnote reference needs two characters of
     * lookahead.
     */
    footnote_reference: ($) => seq(
      field('label', alias($._footnote_reference, $.footnote_label)),
    ),

    bracket_literal: ($) => $._bracket_literal,

    autolink: ($) => choice(
      seq('<', field('uri', $.uri_autolink), '>'),
      seq('<', field('email', $.email_autolink), '>'),
    ),

    uri_autolink: (_) => /[a-zA-Z][a-zA-Z0-9+.-]{1,31}:[^<>\s]*/,
    email_autolink: (_) => /[^<>\s@]+@[^<>\s@]+\.[^<>\s@]+/,

    emphasis: ($) => seq(
      field('open', $._emphasis_open),
      repeat1($._inline_element),
      field('close', $._emphasis_close),
    ),

    strong_emphasis: ($) => seq(
      field('open', $._strong_open),
      repeat1($._inline_element),
      field('close', $._strong_close),
    ),

    backslash_escape: (_) => /\\[!-/:-@\[-`{-~]/,

    entity_reference: (_) =>
      /&(#[0-9]{1,7}|#[xX][0-9a-fA-F]{1,6}|[a-zA-Z][a-zA-Z0-9]{1,31});/,

    html_tag: (_) => choice(
      /<\/?[a-zA-Z][^<>\n]*>/,
      /<!--[^\n]*?-->/,
    ),

    /* `_sectionN` groups a level-N heading with its contents.  Levels are
     * spelled out because a section has to know whether the next heading closes
     * it, which means the level has to be in the parse state. */
    ...Object.fromEntries([1, 2, 3, 4, 5, 6].flatMap((n) => [
      [`_section${n}`, ($) => seq($[`_heading${n}`], repeat($._block_not_section))],
      [`_heading${n}`, ($) => alias(
        seq(
          field('marker', alias($[`_atx_h${n}_marker`], $.atx_heading_marker)),
          optional(field('content', $.inline)),
          $._line_end,
        ),
        $.atx_heading,
      )],
    ])),
  },
});
