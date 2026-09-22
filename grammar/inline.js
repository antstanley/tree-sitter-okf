/**
 * @file The inline layer of the OKF markdown profile.
 * @license MIT
 *
 * Merged from tree-sitter-markdown `tree-sitter-markdown-inline/grammar.js`
 * (see MERGE.md, deltas I*).  Upstream parses inlines in a *second* grammar,
 * injected into the block grammar's `inline` nodes.  Here both layers live in
 * one grammar, so a single query can join a frontmatter `sources[].id` to a
 * footnote label in the body (spec §1.1(3)).
 *
 * The only structural change that merging forces is in `_soft_line_break`: a
 * line break inside a paragraph is now the block scanner's `_soft_line_ending`
 * followed by the next line's `block_continuation` (the `> ` of a block quote,
 * the indentation of a list item, ...), where upstream saw a bare newline in
 * text the injection had already stripped.
 *
 * Upstream: https://github.com/tree-sitter-grammars/tree-sitter-markdown
 * Copyright (c) 2021 Matthias Deiml, MIT License.
 */

/// <reference types="tree-sitter-cli/dsl" />

const common = require('./common');

// Levels used for dynamic precedence.  Ideally
// n * PRECEDENCE_LEVEL_EMPHASIS > PRECEDENCE_LEVEL_LINK for any n.
const PRECEDENCE_LEVEL_EMPHASIS = 1;
const PRECEDENCE_LEVEL_LINK = common.PRECEDENCE_LEVEL_LINK;
const PRECEDENCE_LEVEL_HTML = 100;

const PUNCTUATION_CHARACTERS_REGEX = common.PUNCTUATION_CHARACTERS_REGEX;

/**
 * Dialect switches (spec D7).  All OFF for OKF v0.2: `[[WikiLink]]` and
 * `#tag` are not part of the format, and turning them on would silently change
 * what a cross-link is.  They are kept, build-gated, so a dialect profile is a
 * build option rather than a fork:
 *
 *     OKF_DIALECT_WIKILINK=1 OKF_DIALECT_TAGS=1 tree-sitter generate
 */
const DIALECT_WIKILINK = !!process.env.OKF_DIALECT_WIKILINK;
const DIALECT_TAGS = !!process.env.OKF_DIALECT_TAGS;

/**
 * The punctuation the scanner may emit itself (delta I2).  `[` is left out:
 * an opening bracket drives most of the GLR forking in the inline layer, and
 * one that can open nothing is `_literal_open_bracket` (delta I3), which
 * records itself.
 */
const RECORDED_PUNCTUATION = common.PUNCTUATION_CHARACTERS_ARRAY
  .filter((c) => !['['].includes(c));

/** External tokens owned by the inline half of `src/scanner.c`. */
const externals = ($) => [
  // Opening and closing delimiters for code spans.  An opening token does not
  // mean the text after it is a code span if there is no closing token.
  $._code_span_start,
  $._code_span_close,

  // Opening and closing delimiters for emphasis.
  $._emphasis_open_star,
  $._emphasis_open_underscore,
  $._emphasis_close_star,
  $._emphasis_close_underscore,

  // For emphasis the scanner needs to know if the last token was whitespace
  // (or the beginning of a line) or punctuation.  These tokens are never
  // emitted; only their validity in a parse state is read.
  $._last_token_whitespace,
  $._last_token_punctuation,

  $._strikethrough_open,
  $._strikethrough_close,

  // Emitted for an opening code span delimiter that has no closer, so the run
  // stays literal text instead of derailing the parse.
  $._unclosed_span,

  // OKF: footnote references (GFM).  `_footnote_reference_start` is zero
  // width and only ever emitted after the scanner has verified that a whole
  // `[^label]` follows, so a stray `[^` stays literal text.
  $._footnote_reference_start,
  $.footnote_label,
  $._footnote_reference_end, // its `]`: emitted by the scanner so that
                             // emphasis right after it is decided the same way
                             // on every parse (delta I2)

  // OKF delta I3: a `[` with no `]` anywhere after it in its paragraph can
  // open no link or image.  The scanner says so with this zero-width token
  // just before it, and the parser then does not fork on it (upstream forks
  // on every `[`, which can exhaust tree-sitter's parse versions on text such
  // as `![*c![`).  Hidden, so trees are unchanged.
  $._literal_open_bracket,

  // OKF delta I2: inline whitespace and punctuation are ordinary tokens, but
  // the scanner emits them itself when an emphasis delimiter run follows, so
  // the kind of character before the run is recorded in scanner state.
  // Upstream reads it from the validity of `_last_token_whitespace` /
  // `_last_token_punctuation` alone, which an incremental re-parse loses when
  // it reuses the subtree ending just before the run.  Order matters: see
  // src/scanner.c.
  $._whitespace_ge_2,
  $._whitespace_1,
  ...RECORDED_PUNCTUATION,
];

const rules = {
  ...common.rules,

  // Different kinds of links:
  // * inline links (https://github.github.com/gfm/#inline-link)
  // * full reference links (https://github.github.com/gfm/#full-reference-link)
  // * collapsed reference links (https://github.github.com/gfm/#collapsed-reference-link)
  // * shortcut links (https://github.github.com/gfm/#shortcut-reference-link)
  //
  // Dynamic precedence is distributed as granular as possible to help the
  // parser decide while parsing which branch is the most important.
  code_span: ($) => seq(
    alias($._code_span_start, $.code_span_delimiter),
    repeat(choice($._text_base, '[', ']', seq($._literal_open_bracket, '['), $._soft_line_break, $._html_tag)),
    alias($._code_span_close, $.code_span_delimiter),
  ),

  _link_text: ($) => prec.dynamic(PRECEDENCE_LEVEL_LINK, choice(
    $._link_text_non_empty,
    seq('[', ']'),
  )),
  _link_text_non_empty: ($) => seq('[', alias($._inline_no_link, $.link_text), ']'),
  shortcut_link: ($) => prec.dynamic(PRECEDENCE_LEVEL_LINK, $._link_text_non_empty),
  full_reference_link: ($) => prec.dynamic(2 * PRECEDENCE_LEVEL_LINK, seq(
    $._link_text,
    $.link_label,
  )),
  collapsed_reference_link: ($) => prec.dynamic(PRECEDENCE_LEVEL_LINK, seq(
    $._link_text,
    '[',
    ']',
  )),
  inline_link: ($) => prec.dynamic(PRECEDENCE_LEVEL_LINK, seq(
    $._link_text,
    '(',
    repeat(choice($._whitespace, $._soft_line_break)),
    optional(seq(
      choice(
        seq(
          $.link_destination,
          optional(seq(
            repeat1(choice($._whitespace, $._soft_line_break)),
            $.link_title,
          )),
        ),
        $.link_title,
      ),
      repeat(choice($._whitespace, $._soft_line_break)),
    )),
    ')',
  )),

  // OKF: a GFM footnote reference, `[^label]`.  The label is the join key
  // against frontmatter `sources[].id` (OKF §5.1, per-claim attribution).
  footnote_reference: ($) => seq(
    $._footnote_reference_start,
    '[',
    '^',
    field('label', $.footnote_label),
    alias($._footnote_reference_end, ']'),
    optional($._last_token_punctuation),
  ),

  // Images work exactly like links with a '!' added in front.
  //
  // https://github.github.com/gfm/#images
  image: ($) => choice(
    $._image_inline_link,
    $._image_shortcut_link,
    $._image_full_reference_link,
    $._image_collapsed_reference_link,
  ),
  _image_inline_link: ($) => prec.dynamic(PRECEDENCE_LEVEL_LINK, seq(
    $._image_description,
    '(',
    repeat(choice($._whitespace, $._soft_line_break)),
    optional(seq(
      choice(
        seq(
          $.link_destination,
          optional(seq(
            repeat1(choice($._whitespace, $._soft_line_break)),
            $.link_title,
          )),
        ),
        $.link_title,
      ),
      repeat(choice($._whitespace, $._soft_line_break)),
    )),
    ')',
  )),
  _image_shortcut_link: ($) => prec.dynamic(3 * PRECEDENCE_LEVEL_LINK, $._image_description_non_empty),
  _image_full_reference_link: ($) => prec.dynamic(PRECEDENCE_LEVEL_LINK, seq($._image_description, $.link_label)),
  _image_collapsed_reference_link: ($) => prec.dynamic(PRECEDENCE_LEVEL_LINK, seq($._image_description, '[', ']')),
  _image_description: ($) => prec.dynamic(3 * PRECEDENCE_LEVEL_LINK, choice($._image_description_non_empty, seq('!', '[', prec(1, ']')))),
  _image_description_non_empty: ($) => seq('!', '[', alias($._inline, $.image_description), prec(1, ']')),

  // Autolinks.  Uri autolinks accept schemes of arbitrary length, which does
  // not align with the spec, because the generated lexer gets too large
  // otherwise.
  //
  // https://github.github.com/gfm/#autolinks
  uri_autolink: (_) => /<[a-zA-Z][a-zA-Z0-9+\.\-][a-zA-Z0-9+\.\-]*:[^ \t\r\n<>]*>/,
  email_autolink: (_) =>
    /<[a-zA-Z0-9.!#$%&'*+/=?^_`{|}~-]+@[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?(?:\.[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)*>/,

  // Raw html.  As with html blocks no additional structure is emitted; that is
  // best done by injecting a real html grammar.
  //
  // https://github.github.com/gfm/#raw-html
  _html_tag: ($) => choice($._open_tag, $._closing_tag, $._html_comment, $._processing_instruction, $._declaration, $._cdata_section),
  _open_tag: ($) => prec.dynamic(PRECEDENCE_LEVEL_HTML, seq('<', $._tag_name, repeat($._attribute), repeat(choice($._whitespace, $._soft_line_break)), optional('/'), '>')),
  _closing_tag: ($) => prec.dynamic(PRECEDENCE_LEVEL_HTML, seq('<', '/', $._tag_name, repeat(choice($._whitespace, $._soft_line_break)), '>')),
  _tag_name: ($) => seq($._word_no_digit, repeat(choice($._word_no_digit, $._digits, '-'))),
  _attribute: ($) => seq(repeat1(choice($._whitespace, $._soft_line_break)), $._attribute_name, repeat(choice($._whitespace, $._soft_line_break)), '=', repeat(choice($._whitespace, $._soft_line_break)), $._attribute_value),
  _attribute_name: (_) => /[a-zA-Z_:][a-zA-Z0-9_\.:\-]*/,
  _attribute_value: ($) => choice(
    /[^ \t\r\n"'=<>`]+/,
    seq("'", repeat(choice($._word, $._whitespace, $._soft_line_break, common.punctuation_without($, ["'"]))), "'"),
    seq('"', repeat(choice($._word, $._whitespace, $._soft_line_break, common.punctuation_without($, ['"']))), '"'),
  ),
  _html_comment: ($) => prec.dynamic(PRECEDENCE_LEVEL_HTML, seq(
    '<!--',
    optional(seq(
      choice(
        $._word,
        $._whitespace,
        $._soft_line_break,
        common.punctuation_without($, ['-', '>']),
        seq(
          '-',
          common.punctuation_without($, ['>']),
        ),
      ),
      repeat(prec.right(choice(
        $._word,
        $._whitespace,
        $._soft_line_break,
        common.punctuation_without($, ['-']),
        seq(
          '-',
          choice(
            $._word,
            $._whitespace,
            $._soft_line_break,
            common.punctuation_without($, ['-']),
          ),
        ),
      ))),
    )),
    '-->',
  )),
  _processing_instruction: ($) => prec.dynamic(PRECEDENCE_LEVEL_HTML, seq(
    '<?',
    repeat(prec.right(choice(
      $._word,
      $._whitespace,
      $._soft_line_break,
      common.punctuation_without($, []),
    ))),
    '?>',
  )),
  _declaration: ($) => prec.dynamic(PRECEDENCE_LEVEL_HTML, seq(
    /<![A-Z]+/,
    choice(
      $._whitespace,
      $._soft_line_break,
    ),
    repeat(prec.right(choice(
      $._word,
      $._whitespace,
      $._soft_line_break,
      common.punctuation_without($, ['>']),
    ))),
    '>',
  )),
  _cdata_section: ($) => prec.dynamic(PRECEDENCE_LEVEL_HTML, seq(
    '<![CDATA[',
    repeat(prec.right(choice(
      $._word,
      $._whitespace,
      $._soft_line_break,
      common.punctuation_without($, []),
    ))),
    ']]>',
  )),

  // A hard line break.
  //
  // https://github.github.com/gfm/#hard-line-breaks
  hard_line_break: ($) => seq(choice('\\', $._whitespace_ge_2), $._soft_line_break),
  _text: ($) => choice($._word, common.punctuation_without($, []), $._whitespace),

  // Whitespace is divided into single whitespaces and multiple whitespaces as
  // hard line breaks need that distinction.
  _whitespace_ge_2: (_) => /\t| [ \t]+/,
  _whitespace_1: (_) => / /,
  _whitespace: ($) => seq(choice($._whitespace_ge_2, $._whitespace_1), optional($._last_token_whitespace)),

  // Other than whitespace, text is tokenized into strings of digits,
  // punctuation characters (see `common.punctuation_without`) and strings of
  // any other characters.  This keeps the number of lexer states small, which
  // is what makes the conflicts below workable.
  _word: ($) => choice($._word_no_digit, $._digits),
  _word_no_digit: (_) => new RegExp('[^' + PUNCTUATION_CHARACTERS_REGEX + ' \\t\\n\\r0-9]+(_+[^' + PUNCTUATION_CHARACTERS_REGEX + ' \\t\\n\\r0-9]+)*'),
  _digits: (_) => /[0-9][0-9_]*/,

  // OKF delta I1: a soft line break is the block scanner's decision that the
  // paragraph continues, plus whatever continuation markers the next line
  // carries.
  _soft_line_break: ($) => seq(
    $._soft_line_ending,
    optional($.block_continuation),
    optional($._last_token_whitespace),
  ),

  _inline_base: ($) => prec.right(repeat1(choice(
    $.image,
    $._soft_line_break,
    $.backslash_escape,
    $.hard_line_break,
    $.uri_autolink,
    $.email_autolink,
    $.entity_reference,
    $.numeric_character_reference,
    $.code_span,
    $.footnote_reference,
    alias($._html_tag, $.html_tag),
    $._text_base,
    DIALECT_TAGS ? $.tag : choice(),
    $._unclosed_span,
  ))),
  _text_base: ($) => choice(
    $._word,
    common.punctuation_without($, ['[', ']']),
    $._whitespace,
    '<!--',
    /<![A-Z]+/,
    '<?',
    '<![CDATA[',
  ),
  _text_inline_no_link: ($) => choice(
    $._text_base,
    $._emphasis_open_star,
    $._emphasis_open_underscore,
    $._unclosed_span,
  ),

  ...(DIALECT_TAGS ? {
    tag: (_) => /#[0-9]*[a-zA-Z_\-\/][a-zA-Z_\-\/0-9]*/,
  } : {}),

  ...(DIALECT_WIKILINK ? {
    wiki_link: ($) => prec.dynamic(2 * PRECEDENCE_LEVEL_LINK, seq(
      '[', '[',
      alias($._wiki_link_destination, $.link_destination),
      optional(seq(
        '|',
        alias($._wiki_link_text, $.link_text),
      )),
      ']', ']',
    )),
    _wiki_link_destination: ($) => repeat1(choice(
      $._word,
      common.punctuation_without($, ['[', ']', '|']),
      $._whitespace,
    )),
    _wiki_link_text: ($) => repeat1(choice(
      $._word,
      common.punctuation_without($, ['[', ']']),
      $._whitespace,
    )),
  } : {}),
};

const precedences = ($) => [
  [$._strong_emphasis_star_no_link, $._inline_element_no_star_no_link],
  [$._strong_emphasis_underscore_no_link, $._inline_element_no_underscore_no_link],
  [$.hard_line_break, $._whitespace],
  [$.hard_line_break, $._text_base],
];

const baseConflicts = ($) => [
  [$._closing_tag, $._text_base],
  [$._open_tag, $._text_base],
  [$._html_comment, $._text_base],
  [$._processing_instruction, $._text_base],
  [$._declaration, $._text_base],
  [$._cdata_section, $._text_base],

  [$._link_text_non_empty, $._inline_element],
  [$._link_text_non_empty, $._inline_element_no_star],
  [$._link_text_non_empty, $._inline_element_no_underscore],
  [$._link_text_non_empty, $._inline_element_no_tilde],
  [$._link_text, $._inline_element],
  [$._link_text, $._inline_element_no_star],
  [$._link_text, $._inline_element_no_underscore],
  [$._link_text, $._inline_element_no_tilde],

  [$._image_description, $._image_description_non_empty, $._text_base],

  [$._image_shortcut_link, $._image_description],
  [$.shortcut_link, $._link_text],
  [$.link_destination, $.link_title],
  [$._link_destination_parenthesis, $.link_title],

  ...(DIALECT_WIKILINK ? [
    [$.wiki_link, $._inline_element],
    [$.wiki_link, $._inline_element_no_star],
    [$.wiki_link, $._inline_element_no_underscore],
    [$.wiki_link, $._inline_element_no_tilde],
  ] : []),
];

/**
 * Generates the inline rule families.  Some inlines have to be parsed
 * differently depending on context: emphasis may not directly contain a bare
 * delimiter of its own kind, and link text may not contain another link.  So
 * `_inline_element` exists in eight variants, one per (link?, delimiter)
 * combination.  Upstream calls this "by far the most ugly part of this code";
 * it is kept verbatim so the upstream conflict analysis still applies.
 */
function addInlineRules(target) {
  const conflicts = [];
  for (const link of [true, false]) {
    const suffix_link = link ? '' : '_no_link';
    for (const delimiter of [false, 'star', 'underscore', 'tilde']) {
      const suffix_delimiter = delimiter ? '_no_' + delimiter : '';
      const suffix = suffix_delimiter + suffix_link;
      target['_inline_element' + suffix] = ($) => {
        let elements = [
          $._inline_base,
          alias($['_emphasis_star' + suffix_link], $.emphasis),
          alias($['_strong_emphasis_star' + suffix_link], $.strong_emphasis),
          alias($['_emphasis_underscore' + suffix_link], $.emphasis),
          alias($['_strong_emphasis_underscore' + suffix_link], $.strong_emphasis),
          alias($['_strikethrough' + suffix_link], $.strikethrough),
        ];
        if (delimiter !== 'star') elements.push($._emphasis_open_star);
        if (delimiter !== 'underscore') elements.push($._emphasis_open_underscore);
        if (delimiter !== 'tilde') elements.push($._strikethrough_open);
        if (link) {
          elements = elements.concat([
            $.shortcut_link,
            $.full_reference_link,
            $.collapsed_reference_link,
            $.inline_link,
            seq(choice('[', ']', seq($._literal_open_bracket, '[')), optional($._last_token_punctuation)),
          ]);
          if (DIALECT_WIKILINK) elements.push($.wiki_link);
        }
        return choice(...elements);
      };
      target['_inline' + suffix] = ($) => repeat1($['_inline_element' + suffix]);
      if (delimiter !== 'star') {
        conflicts.push(['_emphasis_star' + suffix_link, '_inline_element' + suffix_delimiter + suffix_link]);
        conflicts.push(['_emphasis_star' + suffix_link, '_strong_emphasis_star' + suffix_link, '_inline_element' + suffix_delimiter + suffix_link]);
      }
      if (delimiter == 'star' || delimiter == 'underscore') {
        conflicts.push(['_strong_emphasis_' + delimiter + suffix_link, '_inline_element_no_' + delimiter]);
      }
      if (delimiter !== 'underscore') {
        conflicts.push(['_emphasis_underscore' + suffix_link, '_inline_element' + suffix_delimiter + suffix_link]);
        conflicts.push(['_emphasis_underscore' + suffix_link, '_strong_emphasis_underscore' + suffix_link, '_inline_element' + suffix_delimiter + suffix_link]);
      }
      if (delimiter !== 'tilde') {
        conflicts.push(['_strikethrough' + suffix_link, '_inline_element' + suffix_delimiter + suffix_link]);
      }
    }

    target['_strikethrough' + suffix_link] = ($) => prec.dynamic(PRECEDENCE_LEVEL_EMPHASIS, seq(alias($._strikethrough_open, $.emphasis_delimiter), optional($._last_token_punctuation), $['_inline' + '_no_tilde' + suffix_link], alias($._strikethrough_close, $.emphasis_delimiter)));
    target['_emphasis_star' + suffix_link] = ($) => prec.dynamic(PRECEDENCE_LEVEL_EMPHASIS, seq(alias($._emphasis_open_star, $.emphasis_delimiter), optional($._last_token_punctuation), $['_inline' + '_no_star' + suffix_link], alias($._emphasis_close_star, $.emphasis_delimiter)));
    target['_strong_emphasis_star' + suffix_link] = ($) => prec.dynamic(2 * PRECEDENCE_LEVEL_EMPHASIS, seq(alias($._emphasis_open_star, $.emphasis_delimiter), $['_emphasis_star' + suffix_link], alias($._emphasis_close_star, $.emphasis_delimiter)));
    target['_emphasis_underscore' + suffix_link] = ($) => prec.dynamic(PRECEDENCE_LEVEL_EMPHASIS, seq(alias($._emphasis_open_underscore, $.emphasis_delimiter), optional($._last_token_punctuation), $['_inline' + '_no_underscore' + suffix_link], alias($._emphasis_close_underscore, $.emphasis_delimiter)));
    target['_strong_emphasis_underscore' + suffix_link] = ($) => prec.dynamic(2 * PRECEDENCE_LEVEL_EMPHASIS, seq(alias($._emphasis_open_underscore, $.emphasis_delimiter), $['_emphasis_underscore' + suffix_link], alias($._emphasis_close_underscore, $.emphasis_delimiter)));
  }
  return conflicts;
}

const familyConflicts = addInlineRules(rules);

const conflicts = ($) => [
  ...baseConflicts($),
  ...familyConflicts.map((names) => names.map((n) => $[n])),
];

module.exports = {
  RECORDED_PUNCTUATION,
  externals,
  rules,
  precedences,
  conflicts,
  PRECEDENCE_LEVEL_LINK,
};
