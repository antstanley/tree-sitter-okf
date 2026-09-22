/**
 * @file The OKF-YAML frontmatter layer.
 * @license MIT
 *
 * OKF-YAML is a *subset* of YAML 1.2, purpose-built for OKF frontmatter and
 * documented normatively in docs/okf-yaml.md (spec §5.1, D2).  It is never
 * called "YAML" on its own, because it is not: the subset is what the OKF
 * corpus uses plus everything plausible around it, and anything outside it
 * becomes a `yaml_unsupported` leaf rather than an ERROR (spec P4, D8).
 *
 * ## How block structure is carried
 *
 * The scanner owns indentation, in the style of tree-sitter-python.  At every
 * line break it skips the newline, any blank lines and the next line's
 * indentation (they become padding, not tokens) and then emits one zero-width
 * structural token at the start of the next line's content:
 *
 *   `_fm_indent`            the line is deeper than the current block
 *   `_fm_newline`           the line is a sibling in the current block
 *   `_fm_dedent`            the line closes the current block (one per level)
 *   `_fm_sequence_newline`  after `key:` with no value, a `- ` line at the
 *                           key's own column: YAML's zero-indented sequence
 *                           (the official bundles do this for `tags:`,
 *                           `sources:` and `verified:`)
 *
 * A compact block that starts mid-line (`- key: value`, `- - item`) opens an
 * indentation level at its own column without an `_fm_indent`, and is closed
 * by an `_fm_dedent` like any other block.
 *
 * Comment lines, `%` directive lines and the `...` document end marker never
 * affect structure: they are extras, and the structural token is decided at
 * the next real line.
 *
 * ## Scalars
 *
 * Nothing resolves YAML scalar *types* (spec D3).  `plain_scalar` is a
 * byte-faithful span, including folded continuation lines; typing is a
 * replaceable query policy in queries/okf/scalars.scm.
 */

/// <reference types="tree-sitter-cli/dsl" />

/** External tokens owned by the frontmatter half of `src/scanner.c`. */
const externals = ($) => [
  $._fm_open, // '---' + line ending at byte offset 0 (after an optional BOM)
  $._fm_close, // '---' at column 0, + the rest of the line and its line ending
  $._fm_indent,
  $._fm_dedent,
  $._fm_newline,
  $._fm_sequence_newline,
  $._fm_dash, // '-' of a block sequence entry
  $._fm_key, // a plain scalar followed by `:` and whitespace
  $._fm_plain, // a plain scalar in block context, folded continuation lines included
  $._fm_flow_plain, // a plain scalar inside a flow collection
  $._fm_single_quote,
  $._fm_double_quote,
  $._fm_block_scalar, // `|` / `>` header plus the indented body
  $._fm_anchor,
  $._fm_alias,
  $._fm_tag,
  $._fm_flow_mapping_start, // '{' whose collection is balanced
  $._fm_flow_sequence_start, // '[' whose collection is balanced
  $._fm_unsupported, // outside the subset: opaque, never an ERROR
  $._fm_unsupported_rest, // the rest of a line whose first token was unsupported
  // extras
  $.comment,
  $.yaml_directive,
  $.yaml_document_end,
  $._fm_space,
];

/** Extras.  The scanner only ever emits them inside the frontmatter. */
const extras = ($) => [
  $.comment,
  $.yaml_directive,
  $.yaml_document_end,
  $._fm_space,
];

const rules = {
  /**
   * The frontmatter block (spec §4.2).  An unterminated block gets a MISSING
   * closing `---` from error recovery, so a host can say "add a closing
   * delimiter" instead of "parse error" (spec D8).
   */
  frontmatter: ($) => seq(
    alias($._fm_open, '---'),
    optional($._fm_content),
    alias($._fm_close, '---'),
  ),

  /**
   * One block node, optionally preceded by unsupported lines.  Used for the
   * document root and for every nested block, so a bad first line is as
   * harmless as a bad later one.
   */
  _fm_content: ($) => seq(
    repeat(seq($.yaml_unsupported, $._fm_newline)),
    choice(
      $.block_mapping,
      $.block_sequence,
      $._fm_line_node,
      $.yaml_unsupported,
    ),
  ),

  /** A scalar or flow node that stands alone as a block's content. */
  _fm_line_node: ($) => prec.right(seq(
    optional($._fm_properties),
    choice($._fm_inline_node, $.block_scalar),
    repeat(seq($._fm_newline, $.yaml_unsupported)),
  )),

  // --- block mappings ------------------------------------------------------

  block_mapping: ($) => prec.right(seq(
    $.block_mapping_pair,
    repeat(seq($._fm_newline, choice($.block_mapping_pair, $.yaml_unsupported))),
  )),

  block_mapping_pair: ($) => seq(
    optional($._fm_properties),
    field('key', $._fm_key_node),
    ':',
    optional($._fm_pair_value),
  ),

  _fm_key_node: ($) => choice(
    alias($._fm_key, $.plain_scalar),
    $.single_quote_scalar,
    $.double_quote_scalar,
  ),

  /**
   * The value of `key:`.  A same-line node, a block scalar, a nested block on
   * the following lines, or a zero-indented sequence (see the file header).
   * Properties (anchors, tags) may precede any of them, or stand alone; they
   * are children of the pair, never part of its `value` field.
   */
  _fm_pair_value: ($) => choice(
    seq(optional($._fm_properties), field('value', $._fm_inline_node)),
    seq(optional($._fm_properties), field('value', $.block_scalar)),
    seq(optional($._fm_properties), $._fm_indent, field('value', $._fm_content), $._fm_dedent),
    seq(optional($._fm_properties), $._fm_sequence_newline, field('value', $.block_sequence), $._fm_dedent),
    $._fm_properties,
    field('value', $.yaml_unsupported),
  ),

  // --- block sequences -----------------------------------------------------

  block_sequence: ($) => prec.right(seq(
    $.block_sequence_item,
    repeat(seq($._fm_newline, choice($.block_sequence_item, $.yaml_unsupported))),
  )),

  /** `- value`.  The item wraps its value directly (spec §5.3). */
  block_sequence_item: ($) => seq(
    alias($._fm_dash, '-'),
    optional($._fm_item_value),
  ),

  _fm_item_value: ($) => choice(
    seq(optional($._fm_properties), $._fm_inline_node),
    seq(optional($._fm_properties), $.block_scalar),
    seq(optional($._fm_properties), $._fm_indent, $._fm_content, $._fm_dedent),
    $._fm_properties,
    alias($._fm_compact_mapping, $.block_mapping),
    alias($._fm_compact_sequence, $.block_sequence),
    $.yaml_unsupported,
  ),

  /** `- key: value` and its sibling lines, closed by an `_fm_dedent`. */
  _fm_compact_mapping: ($) => seq(
    $.block_mapping_pair,
    repeat(seq($._fm_newline, choice($.block_mapping_pair, $.yaml_unsupported))),
    $._fm_dedent,
  ),

  /** `- - item` and its sibling lines, closed by an `_fm_dedent`. */
  _fm_compact_sequence: ($) => seq(
    $.block_sequence_item,
    repeat(seq($._fm_newline, choice($.block_sequence_item, $.yaml_unsupported))),
    $._fm_dedent,
  ),

  // --- nodes ---------------------------------------------------------------

  _fm_inline_node: ($) => choice(
    alias($._fm_plain, $.plain_scalar),
    $.single_quote_scalar,
    $.double_quote_scalar,
    $.flow_mapping,
    $.flow_sequence,
    $.alias,
  ),

  /**
   * Anchors and tags: recognised, never interpreted (spec D3, Q3).  Opaque
   * means the grammar can never be wrong about them, only incomplete.
   */
  _fm_properties: ($) => prec.right(repeat1(choice($.anchor, $.tag))),
  anchor: ($) => $._fm_anchor,
  tag: ($) => $._fm_tag,
  alias: ($) => $._fm_alias,

  single_quote_scalar: ($) => $._fm_single_quote,
  double_quote_scalar: ($) => $._fm_double_quote,
  block_scalar: ($) => $._fm_block_scalar,

  // --- flow collections ----------------------------------------------------

  flow_mapping: ($) => seq(
    alias($._fm_flow_mapping_start, '{'),
    optional(seq(
      $._flow_entry,
      repeat(seq(',', $._flow_entry)),
      optional(','),
    )),
    '}',
  ),

  flow_sequence: ($) => seq(
    alias($._fm_flow_sequence_start, '['),
    optional(seq(
      $._flow_entry,
      repeat(seq(',', $._flow_entry)),
      optional(','),
    )),
    ']',
  ),

  _flow_entry: ($) => choice($.flow_pair, $._flow_node),

  flow_pair: ($) => seq(
    optional($._fm_properties),
    field('key', $._flow_scalar),
    ':',
    optional(seq(optional($._fm_properties), field('value', $._flow_scalar))),
  ),

  _flow_node: ($) => seq(optional($._fm_properties), $._flow_scalar),

  _flow_scalar: ($) => choice(
    alias($._fm_flow_plain, $.plain_scalar),
    $.single_quote_scalar,
    $.double_quote_scalar,
    $.flow_mapping,
    $.flow_sequence,
    $.alias,
  ),

  /**
   * The never-fail escape hatch (spec §5.1, D8).  Anything outside the subset
   * is one opaque leaf per line, so damage stays local: the body, and every
   * other frontmatter line, still parse.
   */
  yaml_unsupported: ($) => seq($._fm_unsupported, optional($._fm_unsupported_rest)),
};

const conflicts = ($) => [];

module.exports = {
  externals,
  extras,
  rules,
  conflicts,
};
