/**
 * @file Rules shared by the block and inline layers of the OKF markdown profile.
 * @license MIT
 *
 * Merged from tree-sitter-markdown `common/common.js` (see MERGE.md, delta C*).
 * Upstream: https://github.com/tree-sitter-grammars/tree-sitter-markdown
 * Copyright (c) 2021 Matthias Deiml, MIT License.
 */

/// <reference types="tree-sitter-cli/dsl" />

const PUNCTUATION_CHARACTERS_REGEX = '!-/:-@\\[-`\\{-~';
const PUNCTUATION_CHARACTERS_ARRAY = [
  '!', '"', '#', '$', '%', '&', "'", '(', ')', '*', '+', ',', '-', '.', '/', ':', ';', '<',
  '=', '>', '?', '@', '[', '\\', ']', '^', '_', '`', '{', '|', '}', '~',
];

const PRECEDENCE_LEVEL_LINK = 10;

/**
 * Every punctuation character is its own token, optionally followed by the
 * never-emitted `_last_token_punctuation` marker.  The marker's *validity* is
 * what tells the scanner that the previous token was punctuation, which the
 * emphasis flanking rules need (upstream design, kept as is).
 */
function punctuation_without($, chars) {
  return seq(
    choice(...PUNCTUATION_CHARACTERS_ARRAY.filter((c) => !chars.includes(c))),
    optional($._last_token_punctuation),
  );
}

/** A regex matching every HTML5 named character reference. */
function html_entity_regex() {
  // https://html.spec.whatwg.org/multipage/entities.json
  const html_entities = require('./html_entities.json');
  let s = '&(';
  s += Object.keys(html_entities).map((name) => name.substring(1, name.length - 1)).join('|');
  s += ');';
  return new RegExp(s);
}

/** @type {Record<string, ($: GrammarSymbols<any>) => RuleOrLiteral>} */
const rules = {
  // https://github.github.com/gfm/#backslash-escapes
  backslash_escape: ($) => $._backslash_escape,
  _backslash_escape: (_) => new RegExp('\\\\[' + PUNCTUATION_CHARACTERS_REGEX + ']'),

  // https://github.github.com/gfm/#entity-and-numeric-character-references
  entity_reference: (_) => html_entity_regex(),
  numeric_character_reference: (_) => /&#([0-9]{1,7}|[xX][0-9a-fA-F]{1,6});/,

  link_label: ($) => seq('[', repeat1(choice(
    $._text_inline_no_link,
    $.backslash_escape,
    $.entity_reference,
    $.numeric_character_reference,
    $._soft_line_break,
  )), ']'),

  link_destination: ($) => prec.dynamic(PRECEDENCE_LEVEL_LINK, choice(
    seq('<', repeat(choice(
      $._text_no_angle,
      $.backslash_escape,
      $.entity_reference,
      $.numeric_character_reference,
    )), '>'),
    seq(
      choice( // first character is not a '<'
        $._word,
        punctuation_without($, ['<', '(', ')']),
        $.backslash_escape,
        $.entity_reference,
        $.numeric_character_reference,
        $._link_destination_parenthesis,
      ),
      repeat(choice(
        $._word,
        punctuation_without($, ['(', ')']),
        $.backslash_escape,
        $.entity_reference,
        $.numeric_character_reference,
        $._link_destination_parenthesis,
      )),
    ),
  )),
  _link_destination_parenthesis: ($) => seq('(', repeat(choice(
    $._word,
    punctuation_without($, ['(', ')']),
    $.backslash_escape,
    $.entity_reference,
    $.numeric_character_reference,
    $._link_destination_parenthesis,
  )), ')'),
  _text_no_angle: ($) => choice($._word, punctuation_without($, ['<', '>']), $._whitespace),
  link_title: ($) => choice(
    seq('"', repeat(choice(
      $._word,
      punctuation_without($, ['"']),
      $._whitespace,
      $.backslash_escape,
      $.entity_reference,
      $.numeric_character_reference,
      seq($._soft_line_break, optional(seq($._soft_line_break, $._trigger_error))),
    )), '"'),
    seq("'", repeat(choice(
      $._word,
      punctuation_without($, ["'"]),
      $._whitespace,
      $.backslash_escape,
      $.entity_reference,
      $.numeric_character_reference,
      seq($._soft_line_break, optional(seq($._soft_line_break, $._trigger_error))),
    )), "'"),
    seq('(', repeat(choice(
      $._word,
      punctuation_without($, ['(', ')']),
      $._whitespace,
      $.backslash_escape,
      $.entity_reference,
      $.numeric_character_reference,
      seq($._soft_line_break, optional(seq($._soft_line_break, $._trigger_error))),
    )), ')'),
  ),
};

module.exports = {
  PUNCTUATION_CHARACTERS_REGEX,
  PUNCTUATION_CHARACTERS_ARRAY,
  PRECEDENCE_LEVEL_LINK,
  punctuation_without,
  rules,
};
