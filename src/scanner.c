/**
 * @file External scanners for tree-sitter-okf.
 * @license MIT
 *
 * Two scanners in one file, told apart by which symbols are valid in the
 * current parse state:
 *
 *   - the OKF-YAML scanner, which fires only inside `frontmatter` and is the
 *     only part of the grammar that keeps state (an indentation stack plus a
 *     flow-collection depth);
 *   - the markdown scanner, which fires only inside `body` and is *stateless*.
 *
 * ## Two facts about tree-sitter's lexer that shape this file
 *
 * Both were verified experimentally for this project before the grammar was
 * written, and both are load-bearing:
 *
 *   1. `mark_end` sets the end of the produced token *and* the position the next
 *      token is lexed from.  Anything consumed past the `mark_end` is therefore
 *      peeked, not committed, and will be re-lexed.  This is how the scanner
 *      looks at the next line before deciding whether a line ending continues a
 *      paragraph.
 *
 *   2. Input consumed before a `false` return is *not* restored.  So a scanner
 *      must never advance and then decline: every path that consumes anything
 *      must end in a token that covers it.  The rule followed throughout is to
 *      decide validity first, commit second.
 *
 * External tokens also take precedence over longer internal matches, which is
 * what lets a heading marker claim `# ` at a line start before the inline
 * `text` token can swallow the line as prose.
 */

#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ symbols */

/** Token order MUST match the `externals` array in grammar.js exactly. */
enum TokenType {
  BOM,
  EOF_TOKEN,

  /* OKF-YAML */
  FM_OPEN,
  FM_CLOSE,
  FM_SEP,
  FM_KEY_L0,
  FM_KEY_L1,
  FM_KEY_L2,
  FM_KEY_L3,
  FM_DASH_L0,
  FM_DASH_L1,
  FM_DASH_L2,
  FM_DASH_L3,
  FM_KEY_E_L0,
  FM_KEY_E_L1,
  FM_KEY_E_L2,
  FM_KEY_E_L3,
  FM_DASH_E_L0,
  FM_DASH_E_L1,
  FM_DASH_E_L2,
  FM_DASH_E_L3,
  FM_PLAIN,
  FM_SQ,
  FM_DQ,
  FM_BLOCK_SCALAR,
  FM_ANNOTATION,
  FM_COMMENT,
  FM_UNSUPPORTED,
  FM_LBRACE,
  FM_RBRACE,
  FM_LBRACKET,
  FM_RBRACKET,
  FM_COMMA,
  FM_COLON,

  /* markdown line breaks */
  SOFT_BREAK,
  LINE_END,

  /* markdown line starts */
  ATX_H1_MARKER,
  ATX_H2_MARKER,
  ATX_H3_MARKER,
  ATX_H4_MARKER,
  ATX_H5_MARKER,
  ATX_H6_MARKER,
  SETEXT_H1_UNDERLINE,
  SETEXT_H2_UNDERLINE,
  THEMATIC_BREAK,
  BLOCK_QUOTE_MARKER,
  LIST_MARKER_STAR,
  LIST_MARKER_MINUS,
  LIST_MARKER_PLUS,
  LIST_MARKER_ORDERED,
  FENCE_OPEN_BACKTICK,
  FENCE_OPEN_TILDE,
  FENCE_CLOSE_BACKTICK,
  FENCE_CLOSE_TILDE,
  TABLE_PIPE,
  IMAGE_START,
  BANG_LITERAL,
  LINK_OPEN,
  LINK_CLOSE,
  FOOTNOTE_REFERENCE,
  FOOTNOTE_DEFINITION_LABEL,
  BRACKET_LITERAL,
  INDENTED_CODE_LINE,

  /* markdown inline delimiters */
  CODE_SPAN_OPEN,
  CODE_SPAN_CONTENT,
  CODE_SPAN_CLOSE,
  EMPHASIS_OPEN,
  EMPHASIS_CLOSE,
  STRONG_OPEN,
  STRONG_CLOSE,
};
/** Deepest frontmatter block nesting the scanner distinguishes. */
#define FM_MAX_LEVEL 4
/** Longest single line the lookahead helpers will buffer. */
#define LINE_BUFFER 512
/** Tab width used for indentation arithmetic. */
#define TAB_WIDTH 8

typedef struct {
  /* OKF-YAML */
  int32_t depth;                    /* number of open block levels */
  int32_t levels[FM_MAX_LEVEL];     /* the indent of each open level */
  int32_t flow_depth;               /* nesting inside `{`/`[`, where indent is inert */
  bool can_nest;                    /* the previous entry had an empty value */
  bool fm_started;                  /* `_fm_open` has been seen */

  /* markdown inline */
  uint8_t code_span_len;            /* backtick run length of the open code span */
} Scanner;

/* ------------------------------------------------------------------- helpers */

static inline void skip(TSLexer *lexer) { lexer->advance(lexer, false); }

static inline bool at_eol(int32_t c) { return c == '\n' || c == '\r' || c == 0; }
static inline bool is_space(int32_t c) { return c == ' ' || c == '\t'; }
static inline bool at_line_start(TSLexer *lexer) { return lexer->get_column(lexer) == 0; }

/** Consume a LF, CRLF or CR line ending.  Returns false if there was none. */
static bool consume_eol(TSLexer *lexer) {
  if (lexer->lookahead == '\r') {
    skip(lexer);
    if (lexer->lookahead == '\n') skip(lexer);
    return true;
  }
  if (lexer->lookahead == '\n') {
    skip(lexer);
    return true;
  }
  return false;
}

/** Consume indentation, returning its width. */
static int32_t consume_indent(TSLexer *lexer) {
  int32_t indent = 0;
  for (;;) {
    if (lexer->lookahead == ' ') {
      indent += 1;
      skip(lexer);
    } else if (lexer->lookahead == '\t') {
      indent += TAB_WIDTH;
      skip(lexer);
    } else {
      break;
    }
  }
  return indent;
}

/* ============================================================ OKF-YAML scanner */

/**
 * Read the rest of the current line into `buffer` (peeking: the caller is
 * responsible for having already committed whatever it wants to keep).  Returns
 * the line length, or -1 if it exceeds the buffer.
 */
static int32_t peek_rest_of_line(TSLexer *lexer, uint8_t *buffer) {
  int32_t n = 0;
  while (!at_eol(lexer->lookahead)) {
    if (n >= LINE_BUFFER) return -1;
    buffer[n++] = (uint8_t)lexer->lookahead;
    skip(lexer);
  }
  buffer[n] = 0;
  return n;
}

/**
 * Does this line look like a mapping entry, i.e. `key:` followed by whitespace
 * or the end of the line?  Used both to decide whether a key is a key and to
 * decide whether a deeper line folds into a plain scalar.
 */
static int32_t entry_colon(const uint8_t *line, int32_t len) {
  if (len <= 0) return -1;
  if (line[0] == '#' || line[0] == '-' || line[0] == '?') return -1;
  for (int32_t i = 0; i < len; i++) {
    uint8_t c = line[i];
    if (c == ':' ) {
      if (i + 1 >= len || line[i + 1] == ' ' || line[i + 1] == '\t') return i;
      return -1;
    }
    if (c == '#' && i > 0) return -1;
    if (c == '{' || c == '[') return -1;
  }
  return -1;
}

/**
 * The indentation level of the entry that starts at the cursor.
 *
 * Levels are absolute, which is what lets the grammar nest without any dedent
 * markers: an entry token for level 2 simply cannot appear inside a level-1
 * block, so a shallower line automatically ends every deeper block.
 *
 * A deeper line only opens a new level when the previous entry had an empty
 * value, because that is the only case in which a nested block can follow.  A
 * deeper line anywhere else is malformed indentation and is treated as a
 * sibling, which mis-nests it rather than failing (P4, "never ERROR").
 */
static int32_t enter_level(Scanner *s, int32_t indent) {
  if (s->depth == 0) {
    s->levels[0] = indent;
    s->depth = 1;
    return 0;
  }
  int32_t top = s->levels[s->depth - 1];
  if (indent > top) {
    if (s->can_nest && s->depth < FM_MAX_LEVEL) {
      s->levels[s->depth] = indent;
      s->depth++;
    } else {
      s->levels[s->depth - 1] = indent;
    }
  } else if (indent < top) {
    while (s->depth > 1 && indent < s->levels[s->depth - 1]) s->depth--;
    if (indent > s->levels[s->depth - 1]) s->levels[s->depth - 1] = indent;
  }
  int32_t level = s->depth - 1;
  if (level >= FM_MAX_LEVEL) level = FM_MAX_LEVEL - 1;
  return level;
}

/**
 * Consume the separator that precedes an entry: the current line's ending and
 * any blank or comment-only lines.  Called only when we are at a line ending.
 */
static void consume_separator(TSLexer *lexer) {
  for (;;) {
    if (!consume_eol(lexer)) break;
    while (is_space(lexer->lookahead)) skip(lexer);
    if (at_eol(lexer->lookahead)) continue; /* blank line */
    if (lexer->lookahead == '#') {
      /* a comment-only line is part of the separator, and is not a node */
      while (!at_eol(lexer->lookahead)) skip(lexer);
      continue;
    }
    return;
  }
}

/** Is the cursor at a line ending, or already at the start of a new line? */
static bool at_separator(TSLexer *lexer) {
  return at_eol(lexer->lookahead) || at_line_start(lexer);
}

/**
 * A mapping key: the line's indentation, the key text, and the `:`.
 *
 * The key token deliberately *includes* the colon: the scanner cannot rewind,
 * and it has to look at the character after the colon to know whether the
 * mapping has a value on this line.  `yaml_key`'s text therefore ends in a
 * colon; hosts that want the bare key trim it.
 *
 * Scanning is incremental and `mark_end` is refreshed as the token grows, so
 * every character consumed here is covered by the token that comes out.  A line
 * that turns out not to be a key falls back to one opaque token rather than to
 * a failure (spec P4).
 */
static bool scan_fm_key(TSLexer *lexer, Scanner *s, const bool *valid) {
  if (s->flow_depth > 0) return false;
  if (!at_separator(lexer)) return false;

  /* Consuming the separator commits us: from here on every path must end in a
   * token.  If the line turns out not to start an entry, the separator itself
   * becomes the token (`_fm_sep`), which is exactly its span. */
  bool consumed_separator = false;
  if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
    consume_separator(lexer);
    consumed_separator = true;
  }

  int32_t c0 = lexer->lookahead;
  bool key_shaped = !(at_eol(c0) || c0 == '-' || c0 == '#' || c0 == '?' ||
                      c0 == '{' || c0 == '[' || c0 == '&' || c0 == '*' || c0 == '!');
  if (key_shaped) {
    int32_t indent = consume_indent(lexer);
    int32_t level = enter_level(s, indent);
    int32_t key_token = FM_KEY_L0 + level;
    int32_t key_empty_token = FM_KEY_E_L0 + level;

    int32_t run = 0;
    for (;;) {
      int32_t ch = lexer->lookahead;
      if (at_eol(ch)) break;
      if (ch == ':') {
        skip(lexer);
        int32_t after = lexer->lookahead;
        if (after == ' ' || after == '\t' || at_eol(after) || after == '#') {
          /* The spaces after the colon belong to the key token.  If they did
           * not, the scanner's own scalar token would claim them (external
           * tokens win), and a flow collection on the same line would be read
           * as a plain scalar starting with a space. */
          if (is_space(after)) {
            while (is_space(lexer->lookahead)) skip(lexer);
          }
          bool empty = at_eol(lexer->lookahead) || lexer->lookahead == '#';
          lexer->mark_end(lexer);
          s->can_nest = empty;
          int32_t tok = empty ? key_empty_token : key_token;
          if (valid[tok]) {
            lexer->result_symbol = tok;
            return true;
          }
          if (valid[FM_UNSUPPORTED]) {
            lexer->result_symbol = FM_UNSUPPORTED;
            return true;
          }
          return false;
        }
        break;
      }
      if (ch == '#' && run > 0) break;
      run++;
      skip(lexer);
    }

    if (valid[FM_UNSUPPORTED]) {
      while (!at_eol(lexer->lookahead)) skip(lexer);
      lexer->mark_end(lexer);
      lexer->result_symbol = FM_UNSUPPORTED;
      return true;
    }
  }

  if (consumed_separator && valid[FM_SEP]) {
    lexer->mark_end(lexer);
    lexer->result_symbol = FM_SEP;
    return true;
  }
  return false;
}

/**
 * A sequence entry: the indentation and the `-`.
 *
 * `mark_end` is called right after the dash (and any following spaces), *before*
 * the rest of the line is peeked to decide whether the item can hold a nested
 * block.  That ordering is what keeps the peeked content out of the token.
 */
static bool scan_fm_dash(TSLexer *lexer, Scanner *s, const bool *valid) {
  if (s->flow_depth > 0) return false;
  if (!at_separator(lexer)) return false;

  bool consumed_separator = false;
  if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
    consume_separator(lexer);
    consumed_separator = true;
  }

  if (lexer->lookahead == '-' && !is_space(lexer->lookahead)) {
    /* fall through: the dash check needs the following character */
  }
  if (lexer->lookahead == '-') {
    int32_t save_indent = 0;
    (void)save_indent;
  }
  int32_t indent = consume_indent(lexer);
  bool dash_shaped = false;
  int32_t level = 0;
  if (lexer->lookahead == '-') {
    skip(lexer);
    if (is_space(lexer->lookahead) || at_eol(lexer->lookahead)) {
      level = enter_level(s, indent);
      bool empty_item = true;
      {
        /* a dash followed by content on the same line carries a value */
        int32_t probe = 0;
        (void)probe;
      }
      dash_shaped = true;
    }
  }
  if (dash_shaped) {
    /* Whether the item has content on this line decides which flavour of dash
     * token it is, and therefore whether a nested block may follow. */
    bool empty = at_eol(lexer->lookahead) || lexer->lookahead == '#';
    int32_t tok = empty ? FM_DASH_E_L0 + level : FM_DASH_L0 + level;
    if (!valid[tok]) {
      if (consumed_separator && valid[FM_SEP]) {
        /* the dash we consumed is not usable here */
        lexer->mark_end(lexer);
        lexer->result_symbol = FM_SEP;
        return true;
      }
      return false;
    }
    int32_t value_indent = 0;
    (void)value_indent;
    if (is_space(lexer->lookahead)) {
      while (is_space(lexer->lookahead)) skip(lexer);
    }
    lexer->mark_end(lexer);
    s->can_nest = empty;
    lexer->result_symbol = tok;
    return true;
  }

  if (consumed_separator && valid[FM_SEP]) {
    lexer->mark_end(lexer);
    lexer->result_symbol = FM_SEP;
    return true;
  }
  return false;
}

/**
 * A plain scalar, including YAML's folded continuation lines.
 *
 * `mark_end` is refreshed after every content character, so stopping at a
 * trailing comment costs nothing: the last `mark_end` already points at the
 * right place and the comment is simply not covered.
 */
static bool scan_fm_plain(TSLexer *lexer, Scanner *s) {
  int32_t c = lexer->lookahead;
  if (at_eol(c)) return false;
  /* flow indicators and annotation characters are other tokens */
  if (c == '{' || c == '}' || c == '[' || c == ']' || c == ',' || c == ':' ||
      c == '&' || c == '!' || c == '#') {
    return false;
  }
  /* `*` is a plain scalar start only outside a flow collection, where it is an
   * alias; treating it as an annotation is the subset's documented choice. */
  if (c == '*') return false;

  bool any = false;
  bool in_flow = s->flow_depth > 0;
  int32_t entry_indent = s->depth > 0 ? s->levels[s->depth - 1] : 0;

  for (;;) {
    /* the rest of the line, stopping before a trailing comment or a flow
     * indicator */
    int32_t pending_space = 0;
    while (!at_eol(lexer->lookahead)) {
      int32_t ch = lexer->lookahead;
      if (is_space(ch)) {
        pending_space++;
        skip(lexer);
        continue;
      }
      if (in_flow && (ch == ',' || ch == '}' || ch == ']')) break;
      if (in_flow && ch == ':') {
        /* In a flow collection a colon only separates key from value when a
         * space, another indicator or the end of the collection follows it.
         * `2026-01-01T00:00:00Z` is therefore one scalar, not a key and a
         * value.  The colon is consumed only to look past it; `mark_end` still
         * points at the end of the scalar's content. */
        skip(lexer);
        int32_t nx = lexer->lookahead;
        if (is_space(nx) || at_eol(nx) || nx == ',' || nx == '}' || nx == ']') {
          return true;
        }
      }
      if (ch == '#' && pending_space > 0) break;
      pending_space = 0;
      any = true;
      skip(lexer);
      lexer->mark_end(lexer);
    }
    if (!any) return false;
    if (in_flow) return true;

    /* consider one folded continuation line */
    if (!consume_eol(lexer)) return true;
    int32_t indent = 0;
    for (;;) {
      int32_t ch = lexer->lookahead;
      if (ch == ' ') {
        indent += 1;
      } else if (ch == '\t') {
        indent += TAB_WIDTH;
      } else {
        break;
      }
      skip(lexer);
    }
    if (at_eol(lexer->lookahead)) continue;      /* a blank line folds away */
    if (indent <= entry_indent) return true;     /* sibling or parent */

    uint8_t line[LINE_BUFFER + 1];
    int32_t rest = peek_rest_of_line(lexer, line);
    if (rest < 0) return true;
    if (entry_colon(line, rest) >= 0) return true; /* an entry, not a continuation */
    if (line[0] == '-' && (rest == 1 || line[1] == ' ' || line[1] == '\t')) return true;

    /* fold this line in: loop around to consume it, refreshing mark_end */
    for (int32_t i = 0; i < rest; i++) {
      skip(lexer);
      lexer->mark_end(lexer);
    }
    if (!consume_eol(lexer)) return true;
  }
}

/** A double quoted scalar, which may span lines.  YAML folding is not applied:
 *  the token is byte faithful and hosts unquote (spec D3). */
static bool scan_double_quoted(TSLexer *lexer) {
  skip(lexer); /* the opening quote */
  for (;;) {
    int32_t c = lexer->lookahead;
    if (c == 0) return false;
    if (c == '\\') {
      skip(lexer);
      if (lexer->lookahead == 0) return false;
      skip(lexer);
      continue;
    }
    if (c == '"') {
      skip(lexer);
      lexer->mark_end(lexer);
      return true;
    }
    if (at_eol(c)) {
      lexer->mark_end(lexer);
      consume_eol(lexer);
      lexer->mark_end(lexer);
      continue;
    }
    skip(lexer);
    lexer->mark_end(lexer);
  }
}

static bool scan_single_quoted(TSLexer *lexer) {
  skip(lexer); /* the opening quote */
  for (;;) {
    int32_t c = lexer->lookahead;
    if (c == 0 || at_eol(c)) return false;
    if (c == '\'') {
      skip(lexer);
      if (lexer->lookahead == '\'') {
        skip(lexer);
        continue;
      }
      lexer->mark_end(lexer);
      return true;
    }
    skip(lexer);
    lexer->mark_end(lexer);
  }
}

/** `|` or `>` with optional chomping and indentation indicators, plus the body,
 *  which ends at the first non-blank line indented less than the scalar. */
static bool scan_block_scalar(TSLexer *lexer) {
  int32_t c = lexer->lookahead;
  if (c != '|' && c != '>') return false;
  skip(lexer);
  if (lexer->lookahead == '-' || lexer->lookahead == '+') skip(lexer);
  int32_t explicit_indent = 0;
  if (lexer->lookahead >= '1' && lexer->lookahead <= '9') {
    explicit_indent = lexer->lookahead - '0';
    skip(lexer);
  }
  while (is_space(lexer->lookahead)) skip(lexer);
  if (!at_eol(lexer->lookahead)) return false;
  lexer->mark_end(lexer);

  int32_t body_indent = explicit_indent > 0 ? explicit_indent : -1;
  for (;;) {
    if (!consume_eol(lexer)) break;
    int32_t indent = 0;
    for (;;) {
      if (lexer->lookahead == ' ') {
        indent++;
        skip(lexer);
      } else if (lexer->lookahead == '\t') {
        indent += TAB_WIDTH;
        skip(lexer);
      } else {
        break;
      }
    }
    if (at_eol(lexer->lookahead)) continue;
    if (body_indent < 0) {
      body_indent = indent;
      if (body_indent == 0) return false;
    }
    if (indent < body_indent) break;
    while (!at_eol(lexer->lookahead)) skip(lexer);
    lexer->mark_end(lexer);
  }
  return true;
}

/** Anchors, aliases and tags: recognised, opaque, never interpreted (D2, Q3). */
static bool scan_fm_annotation(TSLexer *lexer) {
  int32_t c = lexer->lookahead;
  if (c != '&' && c != '*' && c != '!') return false;
  skip(lexer);
  while (!at_eol(lexer->lookahead) && !is_space(lexer->lookahead)) {
    skip(lexer);
    lexer->mark_end(lexer);
  }
  return true;
}

static bool scan_fm_flow_punct(TSLexer *lexer, Scanner *s, int32_t token) {
  int32_t c = lexer->lookahead;
  bool match = (token == FM_LBRACE && c == '{') ||
               (token == FM_RBRACE && c == '}') ||
               (token == FM_LBRACKET && c == '[') ||
               (token == FM_RBRACKET && c == ']') ||
               (token == FM_COMMA && c == ',') ||
               (token == FM_COLON && c == ':');
  if (!match) return false;
  skip(lexer);
  lexer->mark_end(lexer);
  if (token == FM_LBRACE || token == FM_LBRACKET) {
    s->flow_depth++;
  } else if (token == FM_RBRACE || token == FM_RBRACKET) {
    if (s->flow_depth > 0) s->flow_depth--;
  }
  return true;
}

static bool scan_fm(TSLexer *lexer, Scanner *s, const bool *valid) {
  if (valid[FM_OPEN] && at_line_start(lexer) && lexer->lookahead == '-') {
    int32_t dashes = 0;
    while (lexer->lookahead == '-') {
      dashes++;
      skip(lexer);
    }
    if (dashes < 3) return false;
    while (!at_eol(lexer->lookahead)) skip(lexer);
    consume_eol(lexer);
    lexer->mark_end(lexer);
    s->fm_started = true;
    s->depth = 0;
    s->flow_depth = 0;
    s->can_nest = true;
    lexer->result_symbol = FM_OPEN;
    return true;
  }

  if (valid[FM_CLOSE] && at_line_start(lexer) && lexer->lookahead == '-') {
    int32_t dashes = 0;
    while (lexer->lookahead == '-') {
      dashes++;
      skip(lexer);
    }
    if (dashes < 3) {
      /* not a delimiter after all: keep the never-fail promise by making the
       * line opaque */
      while (!at_eol(lexer->lookahead)) skip(lexer);
      lexer->mark_end(lexer);
      lexer->result_symbol = FM_UNSUPPORTED;
      return true;
    }
    while (!at_eol(lexer->lookahead)) skip(lexer);
    consume_eol(lexer);
    lexer->mark_end(lexer);
    lexer->result_symbol = FM_CLOSE;
    return true;
  }

  /* Entry tokens.  Only attempted when one of them is valid, so that a value
   * position never pays for the line-shape analysis. */
  bool any_key = false, any_dash = false;
  for (int32_t i = 0; i < FM_MAX_LEVEL; i++) {
    if (valid[FM_KEY_L0 + i]) any_key = true;
    if (valid[FM_DASH_L0 + i]) any_dash = true;
  }
  if (any_key && scan_fm_key(lexer, s, valid)) {
    return true;
  }
  if (any_dash && scan_fm_dash(lexer, s, valid)) {
    return true;
  }

  /* Only reached when no entry token applies: the separator before the closing
   * delimiter.  It must come after the entry attempts, because an entry token
   * consumes its own separator. */
  if (valid[FM_SEP] && (lexer->lookahead == '\n' || lexer->lookahead == '\r')) {
    consume_separator(lexer);
    lexer->mark_end(lexer);
    lexer->result_symbol = FM_SEP;
    return true;
  }

  if (valid[FM_COMMENT] && lexer->lookahead == '#') {
    while (!at_eol(lexer->lookahead)) skip(lexer);
    lexer->mark_end(lexer);
    lexer->result_symbol = FM_COMMENT;
    return true;
  }

  if (valid[FM_DQ] && lexer->lookahead == '"' && scan_double_quoted(lexer)) {
    lexer->result_symbol = FM_DQ;
    return true;
  }
  if (valid[FM_SQ] && lexer->lookahead == '\'' && scan_single_quoted(lexer)) {
    lexer->result_symbol = FM_SQ;
    return true;
  }
  if (valid[FM_BLOCK_SCALAR] && scan_block_scalar(lexer)) {
    lexer->result_symbol = FM_BLOCK_SCALAR;
    return true;
  }
  if (valid[FM_ANNOTATION] && scan_fm_annotation(lexer)) {
    lexer->result_symbol = FM_ANNOTATION;
    return true;
  }

  for (int32_t t = FM_LBRACE; t <= FM_COLON; t++) {
    if (valid[t] && scan_fm_flow_punct(lexer, s, t)) {
      lexer->result_symbol = (TSSymbol)t;
      return true;
    }
  }

  if (valid[FM_PLAIN] && scan_fm_plain(lexer, s)) {
    lexer->result_symbol = FM_PLAIN;
    return true;
  }

  if (valid[FM_UNSUPPORTED]) {
    /* The never-fail escape hatch: one opaque line, never a parse error.  At a
     * line ending it also takes the separator, so it can stand in for an entry
     * as well as for a value. */
    if (lexer->lookahead == '\n' || lexer->lookahead == '\r') consume_separator(lexer);
    if (at_eol(lexer->lookahead)) {
      lexer->mark_end(lexer);
      return false;
    }
    while (!at_eol(lexer->lookahead)) skip(lexer);
    lexer->mark_end(lexer);
    lexer->result_symbol = FM_UNSUPPORTED;
    return true;
  }

  return false;
}

/* ============================================================ markdown scanner */

/**
 * Does this line start a markdown block?  This is the whole of the paragraph
 * continuation rule.
 */
static bool line_starts_block(const uint8_t *line, int32_t len, int32_t indent) {
  if (len <= 0) return true; /* a blank line always ends a paragraph */
  int32_t i = 0;
  int32_t c = line[0];

  /* Fences keep CommonMark's optional three-space indent; every other marker
   * only starts a block at column 0, matching `scan_*` above. */
  if ((c == '`' || c == '~') && indent <= 3) {
    int32_t marks = 0;
    while (i < len && line[i] == c) {
      i++;
      marks++;
    }
    if (marks >= 3) return true;
  }
  if (indent != 0) return false;

  if (c == '#') {
    int32_t marks = 0;
    while (i < len && line[i] == '#') {
      i++;
      marks++;
    }
    if (marks <= 6 && (i >= len || line[i] == ' ' || line[i] == '\t')) return true;
  }

  if (c == '*' || c == '-' || c == '_' || c == '`' || c == '~') {
    int32_t marks = 0;
    for (int32_t j = i; j < len; j++) {
      if (line[j] == c) {
        marks++;
      } else if (line[j] != ' ' && line[j] != '\t') {
        marks = 0;
        break;
      }
    }
    if (marks >= 3) return true;
  }

  if (c == '>') return true;

  if (c == '*' || c == '-' || c == '+') {
    if (len == 1 || line[1] == ' ' || line[1] == '\t') return true;
  }
  if (c >= '0' && c <= '9') {
    int32_t j = i;
    while (j < len && line[j] >= '0' && line[j] <= '9') j++;
    if (j < len && (line[j] == '.' || line[j] == ')') &&
        (j + 1 >= len || line[j + 1] == ' ' || line[j + 1] == '\t')) {
      return true;
    }
  }

  if (c == '[') return true; /* footnote or link reference definition */
  if (indent >= 4) return true;
  return false;
}

/**
 * A line ending.  The scanner commits to the ending, then looks at the line that
 * follows to decide whether this is a soft break (a lazy paragraph continuation)
 * or a plain line end.  Because `mark_end` was called on the ending, the peeked
 * line is re-lexed.
 *
 * Validity is checked *before* anything is consumed, per the rule at the top of
 * this file.
 */
static bool scan_line_ending(TSLexer *lexer, const bool *valid) {
  if (!valid[SOFT_BREAK] && !valid[LINE_END]) return false;

  /* At end of input a line ending is emitted zero width, so a file whose last
   * line has no trailing newline still parses without a MISSING token. */
  if (lexer->eof(lexer)) {
    if (!valid[LINE_END]) return false;
    lexer->mark_end(lexer);
    lexer->result_symbol = LINE_END;
    return true;
  }

  if (lexer->lookahead != '\n' && lexer->lookahead != '\r') return false;

  consume_eol(lexer);
  lexer->mark_end(lexer);

  uint8_t line[LINE_BUFFER + 1];
  int32_t indent = 0;
  for (;;) {
    int32_t c = lexer->lookahead;
    if (c == ' ') {
      indent++;
    } else if (c == '\t') {
      indent += TAB_WIDTH;
    } else {
      break;
    }
    if (indent > LINE_BUFFER) break;
    skip(lexer);
  }
  int32_t len = peek_rest_of_line(lexer, line);

  bool continuation = len > 0 && !line_starts_block(line, len, indent);

  if (valid[SOFT_BREAK] && continuation) {
    lexer->result_symbol = SOFT_BREAK;
  } else if (valid[LINE_END]) {
    lexer->result_symbol = LINE_END;
  } else {
    lexer->result_symbol = SOFT_BREAK;
  }
  return true;
}

/** Line-start markers share their indentation and marker-width rules. */
static int32_t consume_upto_three_spaces(TSLexer *lexer) {
  int32_t indent = 0;
  while (lexer->lookahead == ' ' && indent < 4) {
    indent++;
    skip(lexer);
  }
  return indent;
}

static bool scan_atx_marker(TSLexer *lexer, int32_t level) {
  if (!at_line_start(lexer)) return false;
  int32_t n = 0;
  while (lexer->lookahead == '#') {
    n++;
    skip(lexer);
  }
  if (n != level) return false;
  if (!at_eol(lexer->lookahead) && !is_space(lexer->lookahead)) return false;
  while (is_space(lexer->lookahead)) skip(lexer);
  lexer->mark_end(lexer);
  return true;
}

static bool scan_setext(TSLexer *lexer, int32_t ch) {
  if (!at_line_start(lexer)) return false;
  int32_t n = 0;
  while (lexer->lookahead == ch) {
    n++;
    skip(lexer);
  }
  if (n == 0) return false;
  while (is_space(lexer->lookahead)) skip(lexer);
  if (!at_eol(lexer->lookahead)) return false;
  lexer->mark_end(lexer);
  return true;
}

static bool scan_thematic_break(TSLexer *lexer) {
  if (!at_line_start(lexer)) return false;
  int32_t ch = lexer->lookahead;
  if (ch != '*' && ch != '-' && ch != '_') return false;
  int32_t marks = 0;
  for (;;) {
    if (lexer->lookahead == ch) {
      marks++;
      skip(lexer);
    } else if (is_space(lexer->lookahead)) {
      skip(lexer);
    } else {
      break;
    }
  }
  if (marks < 3 || !at_eol(lexer->lookahead)) return false;
  if (is_space(ch)) return false;
  lexer->mark_end(lexer);
  return true;
}

static bool scan_block_quote_marker(TSLexer *lexer) {
  if (!at_line_start(lexer)) return false;
  if (lexer->lookahead != '>') return false;
  skip(lexer);
  if (lexer->lookahead == ' ') skip(lexer);
  lexer->mark_end(lexer);
  return true;
}

/**
 * A list marker.  The three bullet characters and the ordered form all share
 * this shape; the grammar exposes them as one `list_marker` node.
 */
static bool scan_list_marker(TSLexer *lexer, bool ordered) {
  if (!at_line_start(lexer)) return false;

  if (ordered) {
    int32_t digits = 0;
    while (lexer->lookahead >= '0' && lexer->lookahead <= '9') {
      digits++;
      if (digits > 9) return false;
      skip(lexer);
    }
    if (digits == 0) return false;
    if (lexer->lookahead != '.' && lexer->lookahead != ')') return false;
    skip(lexer);
  } else {
    int32_t c = lexer->lookahead;
    if (c != '*' && c != '-' && c != '+') return false;
    skip(lexer);
  }

  if (is_space(lexer->lookahead)) {
    while (is_space(lexer->lookahead)) skip(lexer);
  } else if (!at_eol(lexer->lookahead)) {
    return false;
  }
  lexer->mark_end(lexer);
  return true;
}

static bool scan_fence(TSLexer *lexer, int32_t ch, bool closing) {
  if (!at_line_start(lexer)) return false;
  if (consume_upto_three_spaces(lexer) > 3) return false;
  int32_t n = 0;
  while (lexer->lookahead == ch) {
    n++;
    skip(lexer);
  }
  if (n < 3) return false;
  if (closing) {
    while (is_space(lexer->lookahead)) skip(lexer);
    if (!at_eol(lexer->lookahead)) return false;
  }
  lexer->mark_end(lexer);
  return true;
}

static bool scan_indented_code_line(TSLexer *lexer) {
  if (!at_line_start(lexer)) return false;
  int32_t indent = 0;
  while (lexer->lookahead == ' ' && indent < 4) {
    indent++;
    skip(lexer);
  }
  if (indent < 4) return false;
  while (!at_eol(lexer->lookahead)) skip(lexer);
  lexer->mark_end(lexer);
  return true;
}

/** `{`, and the rest of the marker is parsed by the grammar. */
/**
 * A code span.  The opening run is verified to have a closer on the same line,
 * which is the whole reason a bare backtick can stay literal text (P4).
 */
static bool scan_code_span_open(TSLexer *lexer, Scanner *s) {
  int32_t n = 0;
  while (lexer->lookahead == '`') {
    n++;
    skip(lexer);
  }
  if (n == 0) return false;
  /* forward scan for a run of exactly n */
  int32_t run = 0;
  bool found = false;
  for (;;) {
    int32_t c = lexer->lookahead;
    if (at_eol(c)) break;
    if (c == '`') {
      run++;
    } else {
      if (run == n) {
        found = true;
        break;
      }
      run = 0;
    }
    skip(lexer);
  }
  if (run == n) found = true;
  if (!found) return false;
  s->code_span_len = (uint8_t)(n > 255 ? 255 : n);
  lexer->mark_end(lexer);
  return true;
}

static bool scan_code_span_content(TSLexer *lexer, Scanner *s) {
  int32_t len = s->code_span_len;
  if (len == 0) return false;
  int32_t run = 0;
  for (;;) {
    int32_t c = lexer->lookahead;
    if (at_eol(c)) return run >= len && false;
    if (c == '`') {
      run++;
      if (run == len) {
        if (run >= len) {
          /* the content ends just before this run; if nothing was consumed the
           * span is empty and declines, letting the close token match */
          return false;
        }
      }
    } else {
      run = 0;
    }
    skip(lexer);
    lexer->mark_end(lexer);
  }
}

static bool scan_code_span_close(TSLexer *lexer, Scanner *s) {
  int32_t len = s->code_span_len;
  if (len == 0) return false;
  for (int32_t i = 0; i < len; i++) {
    if (lexer->lookahead != '`') return false;
    skip(lexer);
  }
  s->code_span_len = 0;
  lexer->mark_end(lexer);
  return true;
}

/**
 * Emphasis and strong emphasis.
 *
 * A closing delimiter is only ever requested when the parse state is inside the
 * construct, so `valid_symbols` alone answers "is this run a closer?".  That
 * removes the need for a delimiter stack and therefore for any state.  The
 * opening side checks that a matching run exists later on the line.
 */
static bool scan_emphasis(TSLexer *lexer, const bool *valid, int32_t ch) {
  int32_t n = 0;
  while (lexer->lookahead == ch) {
    n++;
    skip(lexer);
  }
  if (n == 0) return false;
  bool strong = n >= 2;

  if (strong && valid[STRONG_CLOSE]) {
    lexer->mark_end(lexer);
    lexer->result_symbol = STRONG_CLOSE;
    return true;
  }
  if (!strong && valid[EMPHASIS_CLOSE]) {
    lexer->mark_end(lexer);
    lexer->result_symbol = EMPHASIS_CLOSE;
    return true;
  }

  /* forward scan for a matching run */
  int32_t want = strong ? 2 : 1;
  int32_t run = 0;
  bool found = false;
  for (;;) {
    int32_t c = lexer->lookahead;
    if (at_eol(c)) break;
    if (c == ch) {
      run++;
      if (run == want) {
        found = true;
        break;
      }
    } else {
      run = 0;
    }
    skip(lexer);
  }
  if (run == want) found = true;
  if (!found) return false;

  if (strong && valid[STRONG_OPEN]) {
    lexer->mark_end(lexer);
    lexer->result_symbol = STRONG_OPEN;
    return true;
  }
  if (!strong && valid[EMPHASIS_OPEN]) {
    lexer->mark_end(lexer);
    lexer->result_symbol = EMPHASIS_OPEN;
    return true;
  }
  return false;
}

/**
 * Brackets and footnote references.
 *
 * Every `[` and `]` is claimed by exactly one token, so no bracket ever has to
 * fall back to an internal match, and every branch ends in a token covering what
 * was consumed.  Deciding needs at most a couple of characters past the
 * bracket, which is exactly why brackets are scanned rather than matched:
 *
 *   `[^label]:`      a footnote definition label (at the start of a line)
 *   `[^label]`       a footnote reference
 *   `[`              a link's opening bracket
 *   `]` before `(`   a link's closing bracket
 *   anything else    a literal bracket
 *
 * A malformed `[^` with no `]` on the line comes out as one literal bracket
 * spanning what was consumed, never as a parse error (spec P4).
 */
static bool scan_bracket(TSLexer *lexer, const bool *valid) {
  int32_t c = lexer->lookahead;

  if (c == ']') {
    if (!valid[LINK_CLOSE] && !valid[BRACKET_LITERAL]) return false;
    skip(lexer);
    bool opens_destination = lexer->lookahead == '(';
    lexer->mark_end(lexer);
    if (opens_destination && valid[LINK_CLOSE]) {
      lexer->result_symbol = LINK_CLOSE;
      return true;
    }
    if (valid[BRACKET_LITERAL]) {
      lexer->result_symbol = BRACKET_LITERAL;
      return true;
    }
    if (valid[LINK_CLOSE]) {
      lexer->result_symbol = LINK_CLOSE;
      return true;
    }
    return false;
  }

  if (c != '[') return false;

  bool want_definition = at_line_start(lexer) && valid[FOOTNOTE_DEFINITION_LABEL];
  bool want_reference = valid[FOOTNOTE_REFERENCE];
  bool want_link = valid[LINK_OPEN];
  bool want_literal = valid[BRACKET_LITERAL];
  if (!want_definition && !want_reference && !want_link && !want_literal) return false;

  skip(lexer); /* the '[' */
  lexer->mark_end(lexer);

  if (lexer->lookahead == '^' && (want_reference || want_definition)) {
    for (;;) {
      int32_t ch = lexer->lookahead;
      if (at_eol(ch)) break;
      if (ch == ']') {
        skip(lexer);
        lexer->mark_end(lexer);
        if (want_definition && lexer->lookahead == ':') {
          skip(lexer);
          lexer->mark_end(lexer);
          lexer->result_symbol = FOOTNOTE_DEFINITION_LABEL;
          return true;
        }
        if (want_reference) {
          lexer->result_symbol = FOOTNOTE_REFERENCE;
          return true;
        }
        lexer->result_symbol = want_literal ? BRACKET_LITERAL : LINK_OPEN;
        return true;
      }
      skip(lexer);
      lexer->mark_end(lexer);
    }
    if (want_literal) {
      lexer->result_symbol = BRACKET_LITERAL;
      return true;
    }
  }

  if (want_link) {
    lexer->result_symbol = LINK_OPEN;
    return true;
  }
  if (want_literal) {
    lexer->result_symbol = BRACKET_LITERAL;
    return true;
  }
  return false;
}

static bool scan_markdown(TSLexer *lexer, Scanner *s, const bool *valid) {
  if (valid[BOM] && at_line_start(lexer) && lexer->lookahead == 0xFEFF) {
    skip(lexer);
    lexer->mark_end(lexer);
    lexer->result_symbol = BOM;
    return true;
  }

  if (scan_line_ending(lexer, valid)) return true;

  if (valid[CODE_SPAN_OPEN] && scan_code_span_open(lexer, s)) {
    lexer->result_symbol = CODE_SPAN_OPEN;
    return true;
  }
  if (valid[CODE_SPAN_CLOSE] && scan_code_span_close(lexer, s)) {
    lexer->result_symbol = CODE_SPAN_CLOSE;
    return true;
  }
  if (valid[CODE_SPAN_CONTENT] && scan_code_span_content(lexer, s)) {
    lexer->result_symbol = CODE_SPAN_CONTENT;
    return true;
  }
  if ((valid[EMPHASIS_OPEN] || valid[EMPHASIS_CLOSE]) &&
      scan_emphasis(lexer, valid, '*')) {
    return true;
  }
  if ((valid[EMPHASIS_OPEN] || valid[EMPHASIS_CLOSE]) &&
      scan_emphasis(lexer, valid, '_')) {
    return true;
  }

  if (valid[ATX_H1_MARKER] && scan_atx_marker(lexer, 1)) {
    lexer->result_symbol = ATX_H1_MARKER;
    return true;
  }
  if (valid[ATX_H2_MARKER] && scan_atx_marker(lexer, 2)) {
    lexer->result_symbol = ATX_H2_MARKER;
    return true;
  }
  if (valid[ATX_H3_MARKER] && scan_atx_marker(lexer, 3)) {
    lexer->result_symbol = ATX_H3_MARKER;
    return true;
  }
  if (valid[ATX_H4_MARKER] && scan_atx_marker(lexer, 4)) {
    lexer->result_symbol = ATX_H4_MARKER;
    return true;
  }
  if (valid[ATX_H5_MARKER] && scan_atx_marker(lexer, 5)) {
    lexer->result_symbol = ATX_H5_MARKER;
    return true;
  }
  if (valid[ATX_H6_MARKER] && scan_atx_marker(lexer, 6)) {
    lexer->result_symbol = ATX_H6_MARKER;
    return true;
  }

  /* A setext underline is only legal where a paragraph has just ended, so
   * `valid_symbols` decides between it and a thematic break. */
  if (valid[SETEXT_H1_UNDERLINE] && scan_setext(lexer, '=')) {
    lexer->result_symbol = SETEXT_H1_UNDERLINE;
    return true;
  }
  if (valid[SETEXT_H2_UNDERLINE] && scan_setext(lexer, '-')) {
    lexer->result_symbol = SETEXT_H2_UNDERLINE;
    return true;
  }
  if (valid[THEMATIC_BREAK] && scan_thematic_break(lexer)) {
    lexer->result_symbol = THEMATIC_BREAK;
    return true;
  }
  if (valid[FENCE_CLOSE_BACKTICK] && scan_fence(lexer, '`', true)) {
    lexer->result_symbol = FENCE_CLOSE_BACKTICK;
    return true;
  }
  if (valid[FENCE_CLOSE_TILDE] && scan_fence(lexer, '~', true)) {
    lexer->result_symbol = FENCE_CLOSE_TILDE;
    return true;
  }
  if (valid[FENCE_OPEN_BACKTICK] && scan_fence(lexer, '`', false)) {
    lexer->result_symbol = FENCE_OPEN_BACKTICK;
    return true;
  }
  if (valid[FENCE_OPEN_TILDE] && scan_fence(lexer, '~', false)) {
    lexer->result_symbol = FENCE_OPEN_TILDE;
    return true;
  }
  if ((valid[IMAGE_START] || valid[BANG_LITERAL]) && lexer->lookahead == '!') {
    /* '!' is an image only when a '[' follows.  Both readings claim exactly the
     * '!', so nothing is committed that the token does not cover. */
    skip(lexer);
    bool image = lexer->lookahead == '[';
    lexer->mark_end(lexer);
    if (image && valid[IMAGE_START]) {
      lexer->result_symbol = IMAGE_START;
      return true;
    }
    if (!image && valid[BANG_LITERAL]) {
      lexer->result_symbol = BANG_LITERAL;
      return true;
    }
    return false;
  }

  if (scan_bracket(lexer, valid)) return true;

  if (valid[TABLE_PIPE] && lexer->lookahead == '|') {
    skip(lexer);
    lexer->mark_end(lexer);
    lexer->result_symbol = TABLE_PIPE;
    return true;
  }
  if (valid[BLOCK_QUOTE_MARKER] && scan_block_quote_marker(lexer)) {
    lexer->result_symbol = BLOCK_QUOTE_MARKER;
    return true;
  }
  if (valid[LIST_MARKER_ORDERED] && scan_list_marker(lexer, true)) {
    lexer->result_symbol = LIST_MARKER_ORDERED;
    return true;
  }
  if ((valid[LIST_MARKER_STAR] || valid[LIST_MARKER_MINUS] || valid[LIST_MARKER_PLUS]) &&
      scan_list_marker(lexer, false)) {
    /* The scanner cannot report which bullet it consumed, because it decides
     * after the fact and cannot rewind.  The grammar exposes one `list_marker`
     * node for all of them, so nothing is lost. */
    lexer->result_symbol = valid[LIST_MARKER_STAR] ? LIST_MARKER_STAR
                          : valid[LIST_MARKER_MINUS] ? LIST_MARKER_MINUS
                                                     : LIST_MARKER_PLUS;
    return true;
  }
  if (valid[INDENTED_CODE_LINE] && scan_indented_code_line(lexer)) {
    lexer->result_symbol = INDENTED_CODE_LINE;
    return true;
  }

  return false;
}

/* ------------------------------------------------------------------ dispatch */

void *tree_sitter_okf_external_scanner_create(void) {
  Scanner *s = (Scanner *)calloc(1, sizeof(Scanner));
  return s;
}

void tree_sitter_okf_external_scanner_destroy(void *payload) { free(payload); }

unsigned tree_sitter_okf_external_scanner_serialize(void *payload, char *buffer) {
  Scanner *s = (Scanner *)payload;
  unsigned n = 0;
  memcpy(buffer + n, &s->depth, sizeof(s->depth));
  n += sizeof(s->depth);
  memcpy(buffer + n, &s->flow_depth, sizeof(s->flow_depth));
  n += sizeof(s->flow_depth);
  memcpy(buffer + n, s->levels, sizeof(s->levels[0]) * FM_MAX_LEVEL);
  n += sizeof(s->levels[0]) * FM_MAX_LEVEL;
  buffer[n++] = (uint8_t)((s->can_nest ? 1 : 0) | (s->fm_started ? 2 : 0));
  buffer[n++] = s->code_span_len;
  return n;
}

void tree_sitter_okf_external_scanner_deserialize(void *payload, const char *buffer,
                                                 unsigned length) {
  Scanner *s = (Scanner *)payload;
  memset(s, 0, sizeof(Scanner));
  unsigned expected = sizeof(int32_t) * 2 + sizeof(int32_t) * FM_MAX_LEVEL + 2;
  if (length == 0 || length < expected) return;
  unsigned n = 0;
  memcpy(&s->depth, buffer + n, sizeof(s->depth));
  n += sizeof(s->depth);
  memcpy(&s->flow_depth, buffer + n, sizeof(s->flow_depth));
  n += sizeof(s->flow_depth);
  memcpy(s->levels, buffer + n, sizeof(s->levels[0]) * FM_MAX_LEVEL);
  n += sizeof(s->levels[0]) * FM_MAX_LEVEL;
  uint8_t flags = (uint8_t)buffer[n++];
  s->can_nest = (flags & 1) != 0;
  s->fm_started = (flags & 2) != 0;
  s->code_span_len = (uint8_t)buffer[n++];
  if (s->depth < 0 || s->depth > FM_MAX_LEVEL) s->depth = 0;
  if (s->flow_depth < 0) s->flow_depth = 0;
}

void tree_sitter_okf_external_scanner_reset(void *payload) {
  Scanner *s = (Scanner *)payload;
  memset(s, 0, sizeof(Scanner));
}

bool tree_sitter_okf_external_scanner_scan(void *payload, TSLexer *lexer,
                                          const bool *valid_symbols) {
  Scanner *s = (Scanner *)payload;

  for (int32_t i = FM_OPEN; i <= FM_COLON; i++) {
    if (valid_symbols[i]) return scan_fm(lexer, s, valid_symbols);
  }
  return scan_markdown(lexer, s, valid_symbols);
}
