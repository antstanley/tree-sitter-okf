/**
 * @file The block layer of the OKF markdown profile.
 * @license MIT
 *
 * Merged from tree-sitter-markdown `tree-sitter-markdown/grammar.js` (see
 * MERGE.md, deltas B*).  Block structure follows the CommonMark spec
 * (https://spec.commonmark.org/0.30/#blocks-and-inlines) with the GFM profile
 * pinned ON: pipe tables, task lists, strikethrough.  There are no
 * compile-time extension flags; that is the point of OKF having its own
 * grammar (spec D4).
 *
 * Differences from upstream that a reader of both files should know about:
 *
 *  - Inline content is parsed in place (`$.inline`, grammar/inline.js) rather
 *    than left as an opaque node for an injection.  Rules that upstream shares
 *    between the layers under one name (`_word`, `_whitespace`, `_line`) are
 *    split: the inline versions keep the upstream names, and the block layer's
 *    raw-text versions for code, info strings and html blocks become
 *    `_blk_word`, `_blk_whitespace` and `_blk_line`.
 *  - `minus_metadata` / `plus_metadata` are gone; OKF frontmatter is its own
 *    layer (grammar/frontmatter.js).
 *  - Footnote definitions are new (GFM; OKF §5.1 per-claim attribution).
 *  - Task list markers and table-cell pipes are scanner tokens, so that
 *    `[x](url)` stays a link and `|` inside a cell is always a separator.
 *
 * Upstream: https://github.com/tree-sitter-grammars/tree-sitter-markdown
 * Copyright (c) 2021 Matthias Deiml, MIT License.
 */

/// <reference types="tree-sitter-cli/dsl" />

const common = require('./common');

const PRECEDENCE_LEVEL_LINK = common.PRECEDENCE_LEVEL_LINK;
const PUNCTUATION_CHARACTERS_REGEX = common.PUNCTUATION_CHARACTERS_REGEX;

/** External tokens owned by the block half of `src/scanner.c`. */
const externals = ($) => [
  // Block structure gets parsed as follows: after every newline
  // (`$._line_ending`) the scanner tries to match as many open blocks as
  // possible.  For example if the last line was part of a block quote it looks
  // for a `>` at the beginning of the next line, and emits a
  // `$.block_continuation` for the matched blocks.  For this process the
  // scanner keeps a stack of currently open blocks.
  //
  // Failing to match every open block does not necessarily mean the unmatched
  // blocks close: the line could be a lazy continuation line
  // (https://github.github.com/gfm/#lazy-continuation-line).
  //
  // When a block does close (because it was not matched or because a closing
  // token was seen) the scanner emits `$._block_close`.
  $._line_ending, // does not contain the newline characters; see `$._newline`
  $._soft_line_ending,
  $._block_close,
  $.block_continuation,

  // Tokens that start a block.  Blocks that always span one line need no
  // `$._block_close`.
  $._block_quote_start,
  $._indented_chunk_start,
  $.atx_h1_marker, // atx headings do not need a `$._block_close`
  $.atx_h2_marker,
  $.atx_h3_marker,
  $.atx_h4_marker,
  $.atx_h5_marker,
  $.atx_h6_marker,
  $.setext_h1_underline, // setext headings do not need a `$._block_close`
  $.setext_h2_underline,
  $._thematic_break, // thematic breaks do not need a `$._block_close`
  $._list_marker_minus,
  $._list_marker_plus,
  $._list_marker_star,
  $._list_marker_parenthesis,
  $._list_marker_dot,
  $._list_marker_minus_dont_interrupt, // list items that may not interrupt a paragraph
  $._list_marker_plus_dont_interrupt,
  $._list_marker_star_dont_interrupt,
  $._list_marker_parenthesis_dont_interrupt,
  $._list_marker_dot_dont_interrupt,
  $._fenced_code_block_start_backtick,
  $._fenced_code_block_start_tilde,
  $._blank_line_start, // does not contain the newline characters

  // Closing backticks or tildes of a fenced code block.  They trigger a
  // `$._close_block`, which in turn triggers a `$._block_close` at the start
  // of the following line.
  $._fenced_code_block_end_backtick,
  $._fenced_code_block_end_tilde,

  $._html_block_1_start,
  $._html_block_1_end,
  $._html_block_2_start,
  $._html_block_3_start,
  $._html_block_4_start,
  $._html_block_5_start,
  $._html_block_6_start,
  $._html_block_7_start,

  // Used when the close of a block is not decided by the scanner.  A
  // `$._block_close` is emitted at the start of the next line.
  $._close_block,

  // Stops the scanner opening indented chunks while a link reference
  // definition's title would be valid.
  $._no_indented_chunk,

  // `$._error` is never valid and is emitted to kill invalid parse branches:
  // when a newline decides a paragraph ended, and when `$._trigger_error` is
  // valid in `$.link_title`.
  $._error,
  $._trigger_error,
  $._eof,

  $._pipe_table_start,
  $._pipe_table_line_ending,

  // OKF delta B5: GFM task list markers, scanned so `[x](url)` stays a link.
  $.task_list_marker_checked,
  $.task_list_marker_unchecked,

  // OKF delta B6: table-row structure.  `|` is a separator whenever a row is
  // being parsed, and the whitespace padding a cell is not part of it.
  $._pipe_table_pipe,
  $._pipe_table_cell_leading_space,
  $._pipe_table_cell_trailing_space,
  $._pipe_table_empty_cell, // zero width, between two adjacent pipes

  // OKF delta B7: the whitespace between an ATX marker and the heading text.
  // A scanner token, because the inline layer's own whitespace token is valid
  // at the same position and the two would otherwise collide.
  $._atx_heading_space,

  // OKF delta B8: GFM footnote definitions.  The start token is zero width
  // and pushes a container block; `_footnote_definition_marker_end` is the
  // `]:` plus the whitespace that follows it.
  $._footnote_definition_start,
  $._footnote_definition_marker_end,
];

const rules = {
  // ---------------------------------------------------------------------------
  // BLOCK STRUCTURE

  // All blocks.  Every block contains a trailing newline.
  _block: ($) => choice(
    $._block_not_section,
    $.section,
  ),
  _block_not_section: ($) => choice(
    alias($._setext_heading1, $.setext_heading),
    alias($._setext_heading2, $.setext_heading),
    $.paragraph,
    $.indented_code_block,
    $.block_quote,
    $.thematic_break,
    $.list,
    $.fenced_code_block,
    $._blank_line,
    $.html_block,
    $.link_reference_definition,
    $.pipe_table,
    $.footnote_definition,
  ),
  section: ($) => choice($._section1, $._section2, $._section3, $._section4, $._section5, $._section6),
  _section1: ($) => prec.right(seq(
    alias($._atx_heading1, $.atx_heading),
    repeat(choice(
      alias(choice($._section6, $._section5, $._section4, $._section3, $._section2), $.section),
      $._block_not_section,
    )),
  )),
  _section2: ($) => prec.right(seq(
    alias($._atx_heading2, $.atx_heading),
    repeat(choice(
      alias(choice($._section6, $._section5, $._section4, $._section3), $.section),
      $._block_not_section,
    )),
  )),
  _section3: ($) => prec.right(seq(
    alias($._atx_heading3, $.atx_heading),
    repeat(choice(
      alias(choice($._section6, $._section5, $._section4), $.section),
      $._block_not_section,
    )),
  )),
  _section4: ($) => prec.right(seq(
    alias($._atx_heading4, $.atx_heading),
    repeat(choice(
      alias(choice($._section6, $._section5), $.section),
      $._block_not_section,
    )),
  )),
  _section5: ($) => prec.right(seq(
    alias($._atx_heading5, $.atx_heading),
    repeat(choice(
      alias($._section6, $.section),
      $._block_not_section,
    )),
  )),
  _section6: ($) => prec.right(seq(
    alias($._atx_heading6, $.atx_heading),
    repeat($._block_not_section),
  )),

  // ---------------------------------------------------------------------------
  // LEAF BLOCKS

  // https://github.github.com/gfm/#thematic-breaks
  thematic_break: ($) => seq($._thematic_break, choice($._newline, $._eof)),

  // https://github.github.com/gfm/#atx-headings
  //
  // OKF: the content is parsed inline in place.  The scanner knows an ATX
  // heading is single-line content (it emitted the marker), so the line ending
  // is never a soft line break.
  _atx_heading1: ($) => prec(1, seq($.atx_h1_marker, optional($._atx_heading_content), $._atx_heading_end)),
  _atx_heading2: ($) => prec(1, seq($.atx_h2_marker, optional($._atx_heading_content), $._atx_heading_end)),
  _atx_heading3: ($) => prec(1, seq($.atx_h3_marker, optional($._atx_heading_content), $._atx_heading_end)),
  _atx_heading4: ($) => prec(1, seq($.atx_h4_marker, optional($._atx_heading_content), $._atx_heading_end)),
  _atx_heading5: ($) => prec(1, seq($.atx_h5_marker, optional($._atx_heading_content), $._atx_heading_end)),
  _atx_heading6: ($) => prec(1, seq($.atx_h6_marker, optional($._atx_heading_content), $._atx_heading_end)),
  _atx_heading_content: ($) => prec(1, choice(
    seq($._atx_heading_space, optional(field('heading_content', $.inline))),
    field('heading_content', $.inline),
  )),
  _atx_heading_end: ($) => choice($._newline, $._eof),

  // https://github.github.com/gfm/#setext-headings
  _setext_heading1: ($) => seq(
    field('heading_content', $.paragraph),
    $.setext_h1_underline,
    choice($._newline, $._eof),
  ),
  _setext_heading2: ($) => seq(
    field('heading_content', $.paragraph),
    $.setext_h2_underline,
    choice($._newline, $._eof),
  ),

  // An indented code block is made up of indented chunks and blank lines.  The
  // indented chunks are handled by the scanner.
  //
  // https://github.github.com/gfm/#indented-code-blocks
  indented_code_block: ($) => prec.right(seq($._indented_chunk, repeat(choice($._indented_chunk, $._blank_line)))),
  _indented_chunk: ($) => seq($._indented_chunk_start, repeat(choice($._blk_line, $._newline)), $._block_close, optional($.block_continuation)),

  // Fenced code blocks are mainly handled by the scanner, which for backtick
  // fences also checks that the info string contains no backtick.
  //
  // https://github.github.com/gfm/#fenced-code-blocks
  fenced_code_block: ($) => prec.right(choice(
    seq(
      alias($._fenced_code_block_start_backtick, $.fenced_code_block_delimiter),
      optional($._blk_whitespace),
      optional($.info_string),
      choice($._newline, $._eof),
      optional($.code_fence_content),
      optional(seq(alias($._fenced_code_block_end_backtick, $.fenced_code_block_delimiter), $._close_block, $._newline)),
      $._block_close,
    ),
    seq(
      alias($._fenced_code_block_start_tilde, $.fenced_code_block_delimiter),
      optional($._blk_whitespace),
      optional($.info_string),
      choice($._newline, $._eof),
      optional($.code_fence_content),
      optional(seq(alias($._fenced_code_block_end_tilde, $.fenced_code_block_delimiter), $._close_block, $._newline)),
      $._block_close,
    ),
  )),
  code_fence_content: ($) => repeat1(choice($._newline, $._blk_line)),
  info_string: ($) => choice(
    seq($.language, repeat(choice($._blk_line, $.backslash_escape, $.entity_reference, $.numeric_character_reference))),
    // OKF: an info string may start with a `,`, which no language contains
    seq(',', repeat(choice($._blk_line, $.backslash_escape, $.entity_reference, $.numeric_character_reference))),
    seq(
      repeat1(choice('{', '}')),
      optional(choice(
        seq($.language, repeat(choice($._blk_line, $.backslash_escape, $.entity_reference, $.numeric_character_reference))),
        seq($._blk_whitespace, repeat(choice($._blk_line, $.backslash_escape, $.entity_reference, $.numeric_character_reference))),
      )),
    ),
  ),
  language: ($) => prec.right(repeat1(choice($._blk_word, common.punctuation_without($, ['{', '}', ',']), $.backslash_escape, $.entity_reference, $.numeric_character_reference))),

  // No nodes are emitted for the kind or structure of an html block; that is
  // best done with an injected html grammar.
  //
  // https://github.github.com/gfm/#html-blocks
  html_block: ($) => prec(1, seq(choice(
    $._html_block_1,
    $._html_block_2,
    $._html_block_3,
    $._html_block_4,
    $._html_block_5,
    $._html_block_6,
    $._html_block_7,
  ))),
  _html_block_1: ($) => build_html_block($, $._html_block_1_start, $._html_block_1_end, true),
  _html_block_2: ($) => build_html_block($, $._html_block_2_start, '-->', true),
  _html_block_3: ($) => build_html_block($, $._html_block_3_start, '?>', true),
  _html_block_4: ($) => build_html_block($, $._html_block_4_start, '>', true),
  _html_block_5: ($) => build_html_block($, $._html_block_5_start, ']]>', true),
  _html_block_6: ($) => build_html_block($, $._html_block_6_start, seq($._newline, $._blank_line), true),
  _html_block_7: ($) => build_html_block($, $._html_block_7_start, seq($._newline, $._blank_line), false),

  // A link reference definition must not be mistaken for a paragraph or an
  // indented chunk; `$._no_indented_chunk` stops the scanner opening an
  // indented chunk where the definition's `$.link_title` would be valid.
  //
  // https://github.github.com/gfm/#link-reference-definitions
  link_reference_definition: ($) => prec.dynamic(PRECEDENCE_LEVEL_LINK, seq(
    optional($._whitespace),
    $.link_label,
    ':',
    optional(seq(optional($._whitespace), optional(seq($._soft_line_break, optional($._whitespace))))),
    $.link_destination,
    optional(prec.dynamic(2 * PRECEDENCE_LEVEL_LINK, seq(
      choice(
        seq($._whitespace, optional(seq($._soft_line_break, optional($._whitespace)))),
        seq($._soft_line_break, optional($._whitespace)),
      ),
      optional($._no_indented_chunk),
      $.link_title,
    ))),
    choice($._newline, $._soft_line_break, $._eof),
  )),

  // A paragraph.  Whether a line ending continues the paragraph is decided by
  // the scanner, which looks at the next line: if it starts a block that can
  // interrupt a paragraph, the ending is a `_line_ending`, otherwise it is a
  // `_soft_line_ending` inside the inline content.
  //
  // https://github.github.com/gfm/#paragraphs
  paragraph: ($) => seq($.inline, choice($._newline, $._eof)),

  // A blank line including the following newline.
  //
  // https://github.github.com/gfm/#blank-lines
  _blank_line: ($) => seq($._blank_line_start, choice($._newline, $._eof)),

  // ---------------------------------------------------------------------------
  // CONTAINER BLOCKS

  // https://github.github.com/gfm/#block-quotes
  block_quote: ($) => seq(
    alias($._block_quote_start, $.block_quote_marker),
    optional($.block_continuation),
    repeat($._block),
    $._block_close,
    optional($.block_continuation),
  ),

  // Lists do not distinguish loose from tight, for efficiency.  A list only
  // contains items with the same kind of marker.
  //
  // https://github.github.com/gfm/#lists
  list: ($) => prec.right(choice(
    $._list_plus,
    $._list_minus,
    $._list_star,
    $._list_dot,
    $._list_parenthesis,
  )),
  _list_plus: ($) => prec.right(repeat1(alias($._list_item_plus, $.list_item))),
  _list_minus: ($) => prec.right(repeat1(alias($._list_item_minus, $.list_item))),
  _list_star: ($) => prec.right(repeat1(alias($._list_item_star, $.list_item))),
  _list_dot: ($) => prec.right(repeat1(alias($._list_item_dot, $.list_item))),
  _list_parenthesis: ($) => prec.right(repeat1(alias($._list_item_parenthesis, $.list_item))),
  // Some list items can not interrupt a paragraph and are marked as such by
  // the scanner.
  list_marker_plus: ($) => choice($._list_marker_plus, $._list_marker_plus_dont_interrupt),
  list_marker_minus: ($) => choice($._list_marker_minus, $._list_marker_minus_dont_interrupt),
  list_marker_star: ($) => choice($._list_marker_star, $._list_marker_star_dont_interrupt),
  list_marker_dot: ($) => choice($._list_marker_dot, $._list_marker_dot_dont_interrupt),
  list_marker_parenthesis: ($) => choice($._list_marker_parenthesis, $._list_marker_parenthesis_dont_interrupt),
  _list_item_plus: ($) => seq(
    $.list_marker_plus,
    optional($.block_continuation),
    $._list_item_content,
    $._block_close,
    optional($.block_continuation),
  ),
  _list_item_minus: ($) => seq(
    $.list_marker_minus,
    optional($.block_continuation),
    $._list_item_content,
    $._block_close,
    optional($.block_continuation),
  ),
  _list_item_star: ($) => seq(
    $.list_marker_star,
    optional($.block_continuation),
    $._list_item_content,
    $._block_close,
    optional($.block_continuation),
  ),
  _list_item_dot: ($) => seq(
    $.list_marker_dot,
    optional($.block_continuation),
    $._list_item_content,
    $._block_close,
    optional($.block_continuation),
  ),
  _list_item_parenthesis: ($) => seq(
    $.list_marker_parenthesis,
    optional($.block_continuation),
    $._list_item_content,
    $._block_close,
    optional($.block_continuation),
  ),
  // List items are closed after two consecutive blank lines.  The task list
  // marker token includes the whitespace after it (delta B5).
  _list_item_content: ($) => choice(
    prec(1, seq(
      $._blank_line,
      $._blank_line,
      $._close_block,
      optional($.block_continuation),
    )),
    repeat1($._block),
    prec(1, seq(
      choice($.task_list_marker_checked, $.task_list_marker_unchecked),
      $.paragraph,
      repeat($._block),
    )),
  ),

  // OKF delta B8: a GFM footnote definition.  A container block, like a list
  // item whose content is indented four columns.
  footnote_definition: ($) => seq(
    $._footnote_definition_start,
    '[',
    '^',
    field('label', $.footnote_label),
    alias($._footnote_definition_marker_end, ']:'),
    optional($.block_continuation),
    repeat($._block),
    $._block_close,
    optional($.block_continuation),
  ),

  // Newlines as in the spec.  Parsing a newline triggers the matching process
  // by making the scanner emit a `$._line_ending`.
  _newline: ($) => seq(
    $._line_ending,
    optional($.block_continuation),
  ),
  // Raw text for code, info strings and html blocks.  Some symbols are single
  // tokens so html blocks are detected properly.
  _blk_line: ($) => prec.right(repeat1(choice($._blk_word, $._blk_whitespace, common.punctuation_without($, [])))),
  _blk_word: (_) => new RegExp('[^' + PUNCTUATION_CHARACTERS_REGEX + ' \\t\\n\\r]+'),
  _blk_whitespace: (_) => /[ \t]+/,

  // ---------------------------------------------------------------------------
  // PIPE TABLES (GFM, pinned ON: `# Schema`, OKF §4.2)

  pipe_table: ($) => prec.right(seq(
    $._pipe_table_start,
    alias($.pipe_table_row, $.pipe_table_header),
    $._newline,
    $.pipe_table_delimiter_row,
    repeat(seq($._pipe_table_newline, optional($.pipe_table_row))),
    choice($._newline, $._eof),
  )),

  _pipe_table_newline: ($) => seq(
    $._pipe_table_line_ending,
    optional($.block_continuation),
  ),

  // OKF: any row the scanner accepts as a delimiter row parses, including one
  // with a leading pipe and no trailing one (`|---`).
  pipe_table_delimiter_row: ($) => seq(
    optional(seq(
      optional($._blk_whitespace),
      $._pipe,
    )),
    $._delimiter_cell,
    repeat(seq($._pipe, $._delimiter_cell)),
    optional(seq($._pipe, optional($._blk_whitespace))),
  ),
  _delimiter_cell: ($) => seq(
    optional($._blk_whitespace),
    $.pipe_table_delimiter_cell,
    optional($._blk_whitespace),
  ),

  pipe_table_delimiter_cell: ($) => seq(
    optional(alias(':', $.pipe_table_align_left)),
    repeat1('-'),
    optional(alias(':', $.pipe_table_align_right)),
  ),

  // A row.  Cells hold inline content; the padding around a cell is a scanner
  // token so the cell node spans exactly its content.  Every cell between two
  // pipes is a node, even an empty or blank one, so a cell's index is its
  // column (`# Schema` tables, OKF §4.2).  Whitespace after the last pipe is
  // not a cell.
  pipe_table_row: ($) => choice(
    seq(
      choice($._pipe, seq($._first_cell, $._pipe)),
      repeat(seq($._cell_after_pipe, $._pipe)),
      optional($._last_cell_after_pipe),
    ),
    $._first_cell, // a row without any pipe is one cell
  ),
  _first_cell: ($) => choice(
    seq(optional($._pipe_table_cell_leading_space), $.pipe_table_cell, optional($._pipe_table_cell_trailing_space)),
    alias($._pipe_table_cell_leading_space, $.pipe_table_cell),
  ),
  _cell_after_pipe: ($) => choice(
    seq(optional($._pipe_table_cell_leading_space), $.pipe_table_cell, optional($._pipe_table_cell_trailing_space)),
    alias($._pipe_table_cell_leading_space, $.pipe_table_cell),
    alias($._pipe_table_empty_cell, $.pipe_table_cell),
  ),
  _last_cell_after_pipe: ($) => choice(
    seq(optional($._pipe_table_cell_leading_space), $.pipe_table_cell, optional($._pipe_table_cell_trailing_space)),
    $._pipe_table_cell_leading_space,
  ),
  _pipe: ($) => alias($._pipe_table_pipe, '|'),

  pipe_table_cell: ($) => seq(optional($._last_token_whitespace), $._inline),
};

const precedences = ($) => [
  [$._setext_heading1, $._block],
  [$._setext_heading2, $._block],
  [$.indented_code_block, $._block],
];

const conflicts = ($) => [
  [$.link_reference_definition],
];

/**
 * General purpose structure for html blocks.  The kinds mostly work the same
 * but have different opening and closing conditions.  Some may not interrupt a
 * paragraph; the scanner handles that.
 */
function build_html_block($, open, close, _interrupt_paragraph) {
  return seq(
    open,
    repeat(choice(
      $._blk_line,
      $._newline,
      seq(close, $._close_block),
    )),
    $._block_close,
    optional($.block_continuation),
  );
}

module.exports = {
  externals,
  rules,
  precedences,
  conflicts,
};
