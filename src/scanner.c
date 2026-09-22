/**
 * @file External scanner for tree-sitter-okf.
 * @license MIT
 *
 * One scanner, three regions of responsibility:
 *
 *   - the OKF-YAML frontmatter scanner (new), which runs only between the
 *     opening and closing `---` and owns indentation there;
 *   - the markdown block scanner, merged from tree-sitter-markdown
 *     `tree-sitter-markdown/src/scanner.c`;
 *   - the markdown inline scanner, merged from tree-sitter-markdown
 *     `tree-sitter-markdown-inline/src/scanner.c`.
 *
 * The frontmatter region and the body region are disjoint: `fm_active` is set
 * by the opening delimiter and cleared by the closing one, and while it is set
 * no markdown logic runs at all (spec §5.2, §9.6).  In the body, the block and
 * inline halves share the stream.  Where both want the same character (`*`,
 * `_`, `` ` ``, `~`, `[`) one merged function decides, because a scanner cannot
 * rewind to let a second function look at input the first one consumed.
 *
 * Every local change to the vendored code is listed in MERGE.md.  Upstream:
 * https://github.com/tree-sitter-grammars/tree-sitter-markdown
 * Copyright (c) 2021 Matthias Deiml, MIT License.
 *
 * Two facts about tree-sitter's lexer that shape this file:
 *
 *   1. The scanner's state is restored from the last *external* token before
 *      every call.  Mutations made by a call that returns false are discarded,
 *      and so is the lexer position.
 *   2. `mark_end` fixes where the token ends; anything consumed after it was
 *      only looked at.  It can be moved forward, never back.
 */

#include "tree_sitter/alloc.h"
#include "tree_sitter/parser.h"

#include <assert.h>
#include <ctype.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

/* ========================================================================
 * Tokens
 *
 * MUST list the externals in exactly the order grammar.js does:
 * grammar/block.js, then grammar/inline.js, then grammar/frontmatter.js.
 * ======================================================================== */

typedef enum {
    /* block (grammar/block.js) */
    LINE_ENDING,
    SOFT_LINE_ENDING,
    BLOCK_CLOSE,
    BLOCK_CONTINUATION,
    BLOCK_QUOTE_START,
    INDENTED_CHUNK_START,
    ATX_H1_MARKER,
    ATX_H2_MARKER,
    ATX_H3_MARKER,
    ATX_H4_MARKER,
    ATX_H5_MARKER,
    ATX_H6_MARKER,
    SETEXT_H1_UNDERLINE,
    SETEXT_H2_UNDERLINE,
    THEMATIC_BREAK,
    LIST_MARKER_MINUS,
    LIST_MARKER_PLUS,
    LIST_MARKER_STAR,
    LIST_MARKER_PARENTHESIS,
    LIST_MARKER_DOT,
    LIST_MARKER_MINUS_DONT_INTERRUPT,
    LIST_MARKER_PLUS_DONT_INTERRUPT,
    LIST_MARKER_STAR_DONT_INTERRUPT,
    LIST_MARKER_PARENTHESIS_DONT_INTERRUPT,
    LIST_MARKER_DOT_DONT_INTERRUPT,
    FENCED_CODE_BLOCK_START_BACKTICK,
    FENCED_CODE_BLOCK_START_TILDE,
    BLANK_LINE_START,
    FENCED_CODE_BLOCK_END_BACKTICK,
    FENCED_CODE_BLOCK_END_TILDE,
    HTML_BLOCK_1_START,
    HTML_BLOCK_1_END,
    HTML_BLOCK_2_START,
    HTML_BLOCK_3_START,
    HTML_BLOCK_4_START,
    HTML_BLOCK_5_START,
    HTML_BLOCK_6_START,
    HTML_BLOCK_7_START,
    CLOSE_BLOCK,
    NO_INDENTED_CHUNK,
    ERROR,
    TRIGGER_ERROR,
    TOKEN_EOF,
    PIPE_TABLE_START,
    PIPE_TABLE_LINE_ENDING,
    TASK_LIST_MARKER_CHECKED,
    TASK_LIST_MARKER_UNCHECKED,
    PIPE_TABLE_PIPE,
    PIPE_TABLE_CELL_LEADING_SPACE,
    PIPE_TABLE_CELL_TRAILING_SPACE,
    PIPE_TABLE_EMPTY_CELL,
    ATX_HEADING_SPACE,
    FOOTNOTE_DEFINITION_START,
    FOOTNOTE_DEFINITION_MARKER_END,

    /* inline (grammar/inline.js) */
    CODE_SPAN_START,
    CODE_SPAN_CLOSE,
    EMPHASIS_OPEN_STAR,
    EMPHASIS_OPEN_UNDERSCORE,
    EMPHASIS_CLOSE_STAR,
    EMPHASIS_CLOSE_UNDERSCORE,
    LAST_TOKEN_WHITESPACE,
    LAST_TOKEN_PUNCTUATION,
    STRIKETHROUGH_OPEN,
    STRIKETHROUGH_CLOSE,
    UNCLOSED_SPAN,
    FOOTNOTE_REFERENCE_START,
    FOOTNOTE_LABEL,
    FOOTNOTE_REFERENCE_END,
    LITERAL_OPEN_BRACKET,
    WHITESPACE_GE_2,
    WHITESPACE_1,
    // The recorded ASCII punctuation characters, in PUNCTUATION order below
    // (grammar/inline.js RECORDED_PUNCTUATION).
    PUNCTUATION_FIRST,
    PUNCTUATION_LAST = PUNCTUATION_FIRST + 30,

    /* frontmatter (grammar/frontmatter.js) */
    FM_OPEN,
    FM_CLOSE,
    FM_INDENT,
    FM_DEDENT,
    FM_NEWLINE,
    FM_SEQUENCE_NEWLINE,
    FM_DASH,
    FM_KEY,
    FM_PLAIN,
    FM_FLOW_PLAIN,
    FM_SINGLE_QUOTE,
    FM_DOUBLE_QUOTE,
    FM_BLOCK_SCALAR,
    FM_ANCHOR,
    FM_ALIAS,
    FM_TAG,
    FM_FLOW_MAPPING_START,
    FM_FLOW_SEQUENCE_START,
    FM_UNSUPPORTED,
    FM_UNSUPPORTED_REST,
    FM_COMMENT,
    FM_DIRECTIVE,
    FM_DOCUMENT_END,
    FM_SPACE,

    TOKEN_TYPE_COUNT
} TokenType;

/* ========================================================================
 * Scanner state
 * ======================================================================== */

/**
 * A block on the block stack (upstream).
 *
 * LIST_ITEM is a list item with minimal indentation (content begins at column
 * 2) while LIST_ITEM_MAX_INDENTATION is a list item with maximal indentation
 * short of being an indented code block.  ANONYMOUS is any block whose close is
 * not handled by the scanner.  FOOTNOTE_DEFINITION is new: a container whose
 * continuation lines are indented four columns (GFM).
 */
typedef enum {
    BLOCK_QUOTE,
    INDENTED_CODE_BLOCK,
    LIST_ITEM,
    LIST_ITEM_1_INDENTATION,
    LIST_ITEM_2_INDENTATION,
    LIST_ITEM_3_INDENTATION,
    LIST_ITEM_4_INDENTATION,
    LIST_ITEM_5_INDENTATION,
    LIST_ITEM_6_INDENTATION,
    LIST_ITEM_7_INDENTATION,
    LIST_ITEM_8_INDENTATION,
    LIST_ITEM_9_INDENTATION,
    LIST_ITEM_10_INDENTATION,
    LIST_ITEM_11_INDENTATION,
    LIST_ITEM_12_INDENTATION,
    LIST_ITEM_13_INDENTATION,
    LIST_ITEM_14_INDENTATION,
    LIST_ITEM_MAX_INDENTATION,
    FENCED_CODE_BLOCK,
    ANONYMOUS,
    FOOTNOTE_DEFINITION,
} Block;

/* Block scanner state bits (`Scanner.state`). */
// Currently matching (at the beginning of a line)
static const uint8_t STATE_MATCHING = 0x1 << 0;
// Last line break was inside a paragraph
static const uint8_t STATE_WAS_SOFT_LINE_BREAK = 0x1 << 1;
// Block should be closed after next line break
static const uint8_t STATE_CLOSE_BLOCK = 0x1 << 4;
// OKF: the current line is single-line content (an ATX heading or a table
// row), so its line ending is never a soft line break.
static const uint8_t STATE_SINGLE_LINE = 0x1 << 5;
// OKF: a zero-width blank line was emitted at end of input (at most once).
static const uint8_t STATE_EOF_BLANK_LINE = 0x1 << 6;
// OKF: the single-line content is a pipe table row, so an unescaped `|`
// ends every inline span (GFM splits cells before parsing inlines).
static const uint8_t STATE_TABLE_ROW = 0x1 << 7;

/* Inline scanner state bits (`Scanner.inline_state`). */
// Current delimiter run is opening
static const uint8_t STATE_EMPHASIS_DELIMITER_IS_OPEN = 0x1 << 2;
// OKF: the rest of the current delimiter run is literal text (its first
// delimiter found no closer in the paragraph, delta I3): each remaining
// delimiter is emitted as punctuation without searching again, which would
// make a long unclosed run quadratic.
static const uint8_t STATE_LITERAL_RUN = 0x1 << 3;

/* The kind of character before a position (`Scanner.prev_class`). */
enum { CLASS_OTHER, CLASS_WHITESPACE, CLASS_PUNCTUATION };

/* The recorded punctuation tokens: grammar/common.js
 * PUNCTUATION_CHARACTERS_ARRAY without `[` (see grammar/inline.js
 * RECORDED_PUNCTUATION). */
static const char PUNCTUATION[] = "!\"#$%&'()*+,-./:;<=>?@\\]^_`{|}~";

/* Frontmatter indentation levels. */
typedef enum {
    FM_LEVEL_BLOCK,    // an ordinary block, opened by `_fm_indent` or mid-line
    FM_LEVEL_ZERO_SEQ, // a sequence at its parent key's own column
} FmLevelKind;

typedef struct {
    int16_t indent;
    uint8_t kind;
} FmLevel;

/* Where the frontmatter scanner is relative to the current line. */
typedef enum {
    FM_MID_LINE,      // after the first token of a line
    FM_BREAK_PENDING, // the next line's structure was decided at the line
                      // break; its line ending and indentation are pending
    FM_LINE_START,    // at the first token of a line
} FmLine;

#define FM_MAX_DEPTH 48
/** Tab width for OKF-YAML indentation (spec Q6).  YAML forbids tabs in
 *  indentation; they are counted, never fatal. */
#define FM_TAB_WIDTH 8

typedef struct Scanner {
    /* --- block (upstream) --- */
    // A stack of open blocks in the current parse state
    struct {
        size_t size;
        size_t capacity;
        Block *items;
    } open_blocks;
    // Parser state flags
    uint8_t state;
    // Number of blocks that have been matched so far.  Only changes during
    // matching and is reset after every line ending
    uint8_t matched;
    // Consumed but "unused" indentation.  Sometimes a tab needs to be "split"
    // to be used in multiple tokens.
    uint8_t indentation;
    // The current column.  Used to decide how many spaces a tab should equal
    uint8_t column;
    // The delimiter length of the currently open fenced code block
    uint8_t fenced_code_block_delimiter_length;
    bool simulate;

    /* --- inline (upstream) --- */
    uint8_t inline_state;
    uint8_t code_span_delimiter_length;
    // The number of characters remaining in the current emphasis delimiter run
    uint8_t num_emphasis_delimiters_left;
    // OKF: what the last scanner token that can precede an emphasis delimiter
    // run was (whitespace, punctuation or other), and the column it ended at.
    // Emphasis decisions use it when the parse state no longer shows it (see
    // `prev_char_class`).  `pending_*` is what the current call will record.
    uint8_t prev_class;
    uint16_t prev_column;
    uint8_t pending_class;
    uint16_t pending_column;
    // Per call: the end column of leading whitespace marked in `md_scan`
    // (0 if none) and whether it was a single space.
    uint16_t leading_ws_column;
    bool leading_ws_single;

    /* --- frontmatter (new) --- */
    bool fm_active;
    bool fm_root_known;
    bool fm_unsupported_rest;
    // The next line is deeper than its block but cannot open one: it is
    // opaque as a whole.
    bool fm_force_unsupported;
    uint8_t fm_line;
    uint8_t fm_depth;
    // The current line's indentation: its column with tabs advancing to the
    // next multiple of FM_TAB_WIDTH, and its length in characters.  A column
    // later on the line is `fm_line_indent + (get_column - fm_line_chars)`,
    // so mid-line columns and line indentation agree when tabs indent.
    int16_t fm_line_indent;
    uint16_t fm_line_chars;
    // Anchors and tags before a node (`&a key: v`): the column of the first
    // one (-1 when none) and whether it started its line.  The node that
    // follows takes its position from them, not from where it itself starts.
    int16_t fm_node_column;
    bool fm_node_line_start;
    FmLevel fm_levels[FM_MAX_DEPTH];
} Scanner;

/* ========================================================================
 * Serialization
 * ======================================================================== */

static size_t roundup_32(size_t x) {
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x++;
    return x;
}

static void push_block(Scanner *s, Block b) {
    if (s->open_blocks.size == s->open_blocks.capacity) {
        s->open_blocks.capacity =
            s->open_blocks.capacity ? s->open_blocks.capacity << 1 : 8;
        void *tmp = ts_realloc(s->open_blocks.items,
                               sizeof(Block) * s->open_blocks.capacity);
        assert(tmp != NULL);
        s->open_blocks.items = tmp;
    }
    s->open_blocks.items[s->open_blocks.size++] = b;
}

static inline Block pop_block(Scanner *s) {
    return s->open_blocks.items[--s->open_blocks.size];
}

#define HEADER_SIZE 22

static unsigned serialize(Scanner *s, char *buffer) {
    unsigned size = 0;
    buffer[size++] = (char)s->state;
    buffer[size++] = (char)s->matched;
    buffer[size++] = (char)s->indentation;
    buffer[size++] = 0; // was the column; see `advance`
    buffer[size++] = (char)s->fenced_code_block_delimiter_length;
    // OKF: serialized states must be canonical, or parse versions that reach
    // the same place never merge (tree-sitter compares these bytes), which
    // costs time and makes every node built meanwhile unreusable.  The
    // open/close bit of a delimiter run means nothing once the run is over.
    buffer[size++] = (char)(s->num_emphasis_delimiters_left
                                ? s->inline_state
                                : (s->inline_state &
                                   ~(STATE_EMPHASIS_DELIMITER_IS_OPEN | STATE_LITERAL_RUN)));
    buffer[size++] = (char)s->code_span_delimiter_length;
    buffer[size++] = (char)s->num_emphasis_delimiters_left;
    buffer[size++] = (char)s->prev_class;
    buffer[size++] = (char)(s->prev_column & 0xff);
    buffer[size++] = (char)((s->prev_column >> 8) & 0xff);
    buffer[size++] = (char)((s->fm_active ? 1 : 0) | (s->fm_root_known ? 2 : 0) |
                            (s->fm_unsupported_rest ? 4 : 0) |
                            (s->fm_force_unsupported ? 8 : 0) |
                            (s->fm_node_line_start ? 16 : 0));
    buffer[size++] = (char)s->fm_line;
    buffer[size++] = (char)s->fm_depth;
    buffer[size++] = (char)(s->fm_line_indent & 0xff);
    buffer[size++] = (char)((s->fm_line_indent >> 8) & 0xff);
    buffer[size++] = (char)(s->fm_line_chars & 0xff);
    buffer[size++] = (char)((s->fm_line_chars >> 8) & 0xff);
    buffer[size++] = (char)(s->fm_node_column & 0xff);
    buffer[size++] = (char)((s->fm_node_column >> 8) & 0xff);
    size_t blocks = s->open_blocks.size;
    // The fixed header plus the frontmatter levels take at most
    // HEADER_SIZE + 3 * FM_MAX_DEPTH bytes; the block stack gets the rest.
    size_t room = TREE_SITTER_SERIALIZATION_BUFFER_SIZE - HEADER_SIZE -
                  3 * (size_t)s->fm_depth;
    if (blocks > room) blocks = room;
    buffer[size++] = (char)(blocks & 0xff);
    buffer[size++] = (char)((blocks >> 8) & 0xff);
    for (uint8_t i = 0; i < s->fm_depth; i++) {
        buffer[size++] = (char)(s->fm_levels[i].indent & 0xff);
        buffer[size++] = (char)((s->fm_levels[i].indent >> 8) & 0xff);
        buffer[size++] = (char)s->fm_levels[i].kind;
    }
    for (size_t i = 0; i < blocks; i++) {
        buffer[size++] = (char)s->open_blocks.items[i];
    }
    return size;
}

static void deserialize(Scanner *s, const char *buffer, unsigned length) {
    s->open_blocks.size = 0;
    s->state = 0;
    s->matched = 0;
    s->indentation = 0;
    s->column = 0;
    s->fenced_code_block_delimiter_length = 0;
    s->inline_state = 0;
    s->code_span_delimiter_length = 0;
    s->num_emphasis_delimiters_left = 0;
    s->prev_class = CLASS_WHITESPACE;
    s->prev_column = 0;
    s->fm_active = false;
    s->fm_root_known = false;
    s->fm_unsupported_rest = false;
    s->fm_force_unsupported = false;
    s->fm_line = FM_MID_LINE;
    s->fm_depth = 1;
    s->fm_line_indent = 0;
    s->fm_line_chars = 0;
    s->fm_node_column = -1;
    s->fm_node_line_start = false;
    s->fm_levels[0].indent = 0;
    s->fm_levels[0].kind = FM_LEVEL_BLOCK;
    if (length < HEADER_SIZE) return;

    size_t size = 0;
    s->state = (uint8_t)buffer[size++];
    s->matched = (uint8_t)buffer[size++];
    s->indentation = (uint8_t)buffer[size++];
    s->column = (uint8_t)buffer[size++];
    s->fenced_code_block_delimiter_length = (uint8_t)buffer[size++];
    s->inline_state = (uint8_t)buffer[size++];
    s->code_span_delimiter_length = (uint8_t)buffer[size++];
    s->num_emphasis_delimiters_left = (uint8_t)buffer[size++];
    s->prev_class = (uint8_t)buffer[size++];
    s->prev_column = (uint16_t)((uint8_t)buffer[size] | ((uint8_t)buffer[size + 1] << 8));
    size += 2;
    uint8_t flags = (uint8_t)buffer[size++];
    s->fm_active = (flags & 1) != 0;
    s->fm_root_known = (flags & 2) != 0;
    s->fm_unsupported_rest = (flags & 4) != 0;
    s->fm_force_unsupported = (flags & 8) != 0;
    s->fm_node_line_start = (flags & 16) != 0;
    s->fm_line = (uint8_t)buffer[size++];
    s->fm_depth = (uint8_t)buffer[size++];
    s->fm_line_indent = (int16_t)((uint8_t)buffer[size] | ((uint8_t)buffer[size + 1] << 8));
    s->fm_line_chars = (uint16_t)((uint8_t)buffer[size + 2] | ((uint8_t)buffer[size + 3] << 8));
    s->fm_node_column = (int16_t)((uint8_t)buffer[size + 4] | ((uint8_t)buffer[size + 5] << 8));
    size += 6;
    size_t blocks = (uint8_t)buffer[size] | ((size_t)(uint8_t)buffer[size + 1] << 8);
    size += 2;
    if (s->fm_depth < 1 || s->fm_depth > FM_MAX_DEPTH) s->fm_depth = 1;
    for (uint8_t i = 0; i < s->fm_depth && size + 3 <= length; i++) {
        s->fm_levels[i].indent =
            (int16_t)((uint8_t)buffer[size] | ((uint8_t)buffer[size + 1] << 8));
        s->fm_levels[i].kind = (uint8_t)buffer[size + 2];
        size += 3;
    }
    if (blocks > length - size) blocks = length - size;
    if (blocks > 0) {
        if (s->open_blocks.capacity < blocks) {
            size_t capacity = roundup_32(blocks);
            void *tmp = ts_realloc(s->open_blocks.items, sizeof(Block) * capacity);
            assert(tmp != NULL);
            s->open_blocks.items = tmp;
            s->open_blocks.capacity = capacity;
        }
        for (size_t i = 0; i < blocks; i++) {
            s->open_blocks.items[i] = (Block)(uint8_t)buffer[size++];
        }
        s->open_blocks.size = blocks;
    }
}

/* ========================================================================
 * Shared helpers
 * ======================================================================== */

// Determines if a character is punctuation as defined by the markdown spec.
static bool is_punctuation(int32_t chr) {
    return (chr >= '!' && chr <= '/') || (chr >= ':' && chr <= '@') ||
           (chr >= '[' && chr <= '`') || (chr >= '{' && chr <= '~');
}

static inline bool is_newline(int32_t c) { return c == '\n' || c == '\r'; }
static inline bool is_blank(int32_t c) { return c == ' ' || c == '\t'; }
static inline bool is_emphasis_delimiter(int32_t c) {
    return c == '*' || c == '_' || c == '~';
}

// The punctuation token for an ASCII punctuation character, or -1.
static int punctuation_token(int32_t c) {
    if (c <= 0 || c > 127) return -1;
    const char *hit = strchr(PUNCTUATION, (int)c);
    return hit ? PUNCTUATION_FIRST + (int)(hit - PUNCTUATION) : -1;
}

// OKF delta I2, at the start of a block: a block-start check consumed exactly
// one punctuation character and then failed.  If an emphasis delimiter run
// follows, emit that character as its punctuation token (recording it, see
// `prev_char_class`) instead of handing it back to the internal lexer.
static bool emit_lone_punctuation(struct Scanner *s, TSLexer *lexer,
                                  const bool *valid_symbols, int32_t c);

// OKF delta I2, at the start of a block: leading whitespace was consumed as
// indentation (and marked, see `md_scan`), then a delimiter run turned out to
// be inline.  Emit the whitespace as the inline token it is, recording it.
static bool emit_leading_whitespace(struct Scanner *s, TSLexer *lexer,
                                    const bool *valid_symbols, uint32_t end_column,
                                    bool single_space);

// Record what the token being emitted means for a delimiter run right after
// it.  Written to the scanner state only if the call emits a token.
static inline void note_prev(Scanner *s, uint8_t cls, uint32_t column) {
    s->pending_class = cls;
    s->pending_column = (uint16_t)(column > 0xffff ? 0xffff : column);
}

static void mark_end(Scanner *s, TSLexer *lexer) {
    if (!s->simulate) {
        lexer->mark_end(lexer);
    }
}

static bool emit_leading_whitespace(Scanner *s, TSLexer *lexer,
                                    const bool *valid_symbols, uint32_t end_column,
                                    bool single_space) {
    TokenType token = single_space ? WHITESPACE_1 : WHITESPACE_GE_2;
    if (s->simulate || !valid_symbols[token]) return false;
    // mark_end is still where the whitespace ended
    note_prev(s, CLASS_WHITESPACE, end_column);
    s->indentation = 0;
    lexer->result_symbol = token;
    return true;
}

static bool emit_lone_punctuation(Scanner *s, TSLexer *lexer,
                                  const bool *valid_symbols, int32_t c) {
    int token = punctuation_token(c);
    if (s->simulate || token < 0 || !valid_symbols[token] ||
        !is_emphasis_delimiter(lexer->lookahead)) {
        return false;
    }
    lexer->mark_end(lexer);
    note_prev(s, CLASS_PUNCTUATION, lexer->get_column(lexer));
    lexer->result_symbol = (TSSymbol)token;
    return true;
}

// Emits the error token, which is never valid, to kill a parse branch:
// 1. when a newline after a line break that ended a paragraph opens no block;
// 2. when a new block starts after a soft line break;
// 3. when `$._trigger_error` is valid, which lets grammar rules stop branches.
static bool error(TSLexer *lexer) {
    lexer->result_symbol = ERROR;
    return true;
}

// Advance the lexer one character.  Tabs count as spaces with tab stop 4
// (https://github.github.com/gfm/#tabs).  OKF: upstream tracked the column
// itself; merged with the inline layer, whose text the internal lexer reads,
// that count drifts and differs between parse versions, which then cannot
// merge.  The real column is asked for when a tab is actually met.
static size_t advance(Scanner *s, TSLexer *lexer) {
    size_t size = 1;
    if (lexer->lookahead == '\t') {
        size = 4 - lexer->get_column(lexer) % 4;
    }
    s->column = 0;
    lexer->advance(lexer, false);
    return size;
}

static void consume_newline(Scanner *s, TSLexer *lexer) {
    if (lexer->lookahead == '\r') {
        advance(s, lexer);
        if (lexer->lookahead == '\n') advance(s, lexer);
    } else if (lexer->lookahead == '\n') {
        advance(s, lexer);
    }
}

/* ========================================================================
 * Markdown block scanner (upstream, with OKF deltas marked "OKF:")
 * ======================================================================== */

// Returns the indentation level which lines of a list item should have at
// minimum.  Only call with list item blocks.
static uint8_t list_item_indentation(Block block) {
    return (uint8_t)(block - LIST_ITEM + 2);
}

#define NUM_HTML_TAG_NAMES_RULE_1 3

static const char *const HTML_TAG_NAMES_RULE_1[NUM_HTML_TAG_NAMES_RULE_1] = {
    "pre", "script", "style"};

#define NUM_HTML_TAG_NAMES_RULE_7 62

static const char *const HTML_TAG_NAMES_RULE_7[NUM_HTML_TAG_NAMES_RULE_7] = {
    "address",  "article",    "aside",  "base",     "basefont", "blockquote",
    "body",     "caption",    "center", "col",      "colgroup", "dd",
    "details",  "dialog",     "dir",    "div",      "dl",       "dt",
    "fieldset", "figcaption", "figure", "footer",   "form",     "frame",
    "frameset", "h1",         "h2",     "h3",       "h4",       "h5",
    "h6",       "head",       "header", "hr",       "html",     "iframe",
    "legend",   "li",         "link",   "main",     "menu",     "menuitem",
    "nav",      "noframes",   "ol",     "optgroup", "option",   "p",
    "param",    "section",    "source", "summary",  "table",    "tbody",
    "td",       "tfoot",      "th",     "thead",    "title",    "tr",
    "track",    "ul"};

// The tokens that can interrupt a paragraph.  Used to decide, by simulating a
// scan of the next line, whether a line ending is a soft line break.
static bool paragraph_interrupt_symbols[TOKEN_TYPE_COUNT];

static void init_paragraph_interrupt_symbols(void) {
    static bool done = false;
    if (done) return;
    const TokenType interrupting[] = {
        BLOCK_QUOTE_START,
        ATX_H1_MARKER,
        ATX_H2_MARKER,
        ATX_H3_MARKER,
        ATX_H4_MARKER,
        ATX_H5_MARKER,
        ATX_H6_MARKER,
        SETEXT_H1_UNDERLINE,
        SETEXT_H2_UNDERLINE,
        THEMATIC_BREAK,
        LIST_MARKER_MINUS,
        LIST_MARKER_PLUS,
        LIST_MARKER_STAR,
        LIST_MARKER_PARENTHESIS,
        LIST_MARKER_DOT,
        FENCED_CODE_BLOCK_START_BACKTICK,
        FENCED_CODE_BLOCK_START_TILDE,
        BLANK_LINE_START,
        HTML_BLOCK_1_START,
        HTML_BLOCK_2_START,
        HTML_BLOCK_3_START,
        HTML_BLOCK_4_START,
        HTML_BLOCK_5_START,
        HTML_BLOCK_6_START,
        PIPE_TABLE_START,
        // OKF: consecutive `[^id]: ...` lines are separate definitions.
        FOOTNOTE_DEFINITION_START,
    };
    for (size_t i = 0; i < sizeof(interrupting) / sizeof(interrupting[0]); i++) {
        paragraph_interrupt_symbols[interrupting[i]] = true;
    }
    done = true;
}

// Try to match the given block, i.e. consume all tokens that belong to it:
// indentation for list items and indented code blocks, '>' for block quotes.
static bool match(Scanner *s, TSLexer *lexer, Block block) {
    switch (block) {
        case INDENTED_CODE_BLOCK:
            while (s->indentation < 4) {
                if (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                    s->indentation += advance(s, lexer);
                } else {
                    break;
                }
            }
            if (s->indentation >= 4 && lexer->lookahead != '\n' &&
                lexer->lookahead != '\r') {
                s->indentation -= 4;
                return true;
            }
            break;
        case LIST_ITEM:
        case LIST_ITEM_1_INDENTATION:
        case LIST_ITEM_2_INDENTATION:
        case LIST_ITEM_3_INDENTATION:
        case LIST_ITEM_4_INDENTATION:
        case LIST_ITEM_5_INDENTATION:
        case LIST_ITEM_6_INDENTATION:
        case LIST_ITEM_7_INDENTATION:
        case LIST_ITEM_8_INDENTATION:
        case LIST_ITEM_9_INDENTATION:
        case LIST_ITEM_10_INDENTATION:
        case LIST_ITEM_11_INDENTATION:
        case LIST_ITEM_12_INDENTATION:
        case LIST_ITEM_13_INDENTATION:
        case LIST_ITEM_14_INDENTATION:
        case LIST_ITEM_MAX_INDENTATION:
        case FOOTNOTE_DEFINITION: {
            uint8_t required =
                block == FOOTNOTE_DEFINITION ? 4 : list_item_indentation(block);
            while (s->indentation < required) {
                if (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                    s->indentation += advance(s, lexer);
                } else {
                    break;
                }
            }
            if (s->indentation >= required) {
                s->indentation -= required;
                return true;
            }
            if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
                s->indentation = 0;
                return true;
            }
            break;
        }
        case BLOCK_QUOTE:
            while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                s->indentation += advance(s, lexer);
            }
            if (lexer->lookahead == '>') {
                advance(s, lexer);
                s->indentation = 0;
                if (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                    s->indentation += advance(s, lexer) - 1;
                }
                return true;
            }
            break;
        case FENCED_CODE_BLOCK:
        case ANONYMOUS:
            return true;
    }
    return false;
}

static bool md_scan(Scanner *s, TSLexer *lexer, const bool *valid_symbols);
static bool block_start_valid(const bool *valid_symbols);

/* ------------------------------------------------------------------------
 * Inline helpers (upstream inline scanner, see "OKF:" for changes)
 * ------------------------------------------------------------------------ */

// OKF: does the text after a newline certainly continue the current paragraph?
// Conservative by design: it answers "yes" only for lines that cannot start a
// block that interrupts a paragraph.  A code span opener is only emitted when
// its closer is reachable through such lines, which guarantees the paragraph
// cannot end while the code span is open.  The lexer is left at the first
// character of the line that was not inspected.
static bool line_certainly_continues(Scanner *s, TSLexer *lexer) {
    size_t indent = 0;
    while (is_blank(lexer->lookahead)) {
        indent += advance(s, lexer);
    }
    int32_t c = lexer->lookahead;
    if (is_newline(c) || lexer->eof(lexer)) return false; // blank line
    if (s->open_blocks.size > 0 && indent == 0) {
        // Inside a container the line would need a continuation marker; a
        // line without one is lazy and fine, but one with `>` is not ours to
        // judge.  Stay conservative.
        if (c == '>') return false;
    }
    if (indent >= 4 && s->open_blocks.size == 0) return true; // indented code cannot interrupt
    switch (c) {
        case '#': case '>': case '=': case '<': case '|': case '~': case '*':
        case '+': case '-': case '_': case '[': case '`':
            return false;
        default:
            break;
    }
    if (c >= '0' && c <= '9') {
        while (lexer->lookahead >= '0' && lexer->lookahead <= '9') advance(s, lexer);
        if (lexer->lookahead == '.' || lexer->lookahead == ')') return false;
    }
    return true;
}

// OKF: is the line at the cursor shaped like a pipe table delimiter row?
static bool line_is_delimiter_row(Scanner *s, TSLexer *lexer) {
    bool dash = false;
    while (!is_newline(lexer->lookahead) && !lexer->eof(lexer)) {
        int32_t c = lexer->lookahead;
        if (c == '-') {
            dash = true;
        } else if (c != '|' && c != ':' && !is_blank(c)) {
            return false;
        }
        advance(s, lexer);
    }
    return dash;
}

// OKF: look ahead for a closing backtick run of exactly `level` backticks that
// is reachable without the paragraph ending (upstream looked to end of input,
// which was the end of the paragraph there because of the injection).
static bool code_span_closer_ahead(Scanner *s, TSLexer *lexer, uint8_t level) {
    bool single_line = (s->state & STATE_SINGLE_LINE) != 0;
    bool table_row = (s->state & STATE_TABLE_ROW) != 0;
    bool on_continuation_line = false;
    bool line_has_pipe = false;
    size_t close_level = 0;
    for (;;) {
        int32_t c = lexer->lookahead;
        if (table_row && close_level != level) {
            // A cell ends at an unescaped `|`, and a code span with it.
            if (c == '|') return false;
            if (c == '\\') {
                close_level = 0;
                advance(s, lexer);
                if (lexer->lookahead == '|') advance(s, lexer);
                continue;
            }
        }
        if (c == '`' && !lexer->eof(lexer)) {
            close_level++;
            advance(s, lexer);
            continue;
        }
        if (close_level == level) {
            if (!on_continuation_line) return true;
            // A continuation line with a pipe could be a table header, which
            // would have ended the paragraph before it.  Check the rest of the
            // line and the next one.
            while (!is_newline(lexer->lookahead) && !lexer->eof(lexer)) {
                if (lexer->lookahead == '|') line_has_pipe = true;
                advance(s, lexer);
            }
            if (!line_has_pipe || lexer->eof(lexer)) return true;
            consume_newline(s, lexer);
            return !line_is_delimiter_row(s, lexer);
        }
        close_level = 0;
        if (lexer->eof(lexer)) return false;
        if (is_newline(c)) {
            if (single_line) return false;
            consume_newline(s, lexer);
            if (line_has_pipe) {
                // the line just finished could have been a table header
                // only if this line is a delimiter row
                // (a delimiter row can never continue a paragraph anyway)
                if (lexer->lookahead == '|' || lexer->lookahead == '-' ||
                    lexer->lookahead == ':' || is_blank(lexer->lookahead)) {
                    return false;
                }
            }
            on_continuation_line = true;
            line_has_pipe = false;
            // A continuation line may itself start with a backtick run: the
            // closer, unless it is long enough to be a fence.
            while (is_blank(lexer->lookahead)) advance(s, lexer);
            if (lexer->lookahead == '`') {
                size_t run = 0;
                while (lexer->lookahead == '`') {
                    run++;
                    advance(s, lexer);
                }
                if (run >= 3) return false;
                close_level = run;
                continue;
            }
            if (!line_certainly_continues(s, lexer)) return false;
            continue;
        }
        if (c == '|' && on_continuation_line) line_has_pipe = true;
        advance(s, lexer);
    }
}

// OKF delta I3: could `closer` occur after the cursor and still be in this
// paragraph?  Answers "no" only when it is certain: the paragraph (or the
// single-line construct) ends first.  Any doubt answers "yes".
static bool closer_possible(Scanner *s, TSLexer *lexer, int32_t closer) {
    bool single_line = (s->state & STATE_SINGLE_LINE) != 0;
    bool table_row = (s->state & STATE_TABLE_ROW) != 0;
    for (;;) {
        if (lexer->eof(lexer)) return false;
        int32_t c = lexer->lookahead;
        if (c == closer) return true;
        if (table_row && c == '|') return false; // the cell ends first
        if (table_row && c == '\\') {
            advance(s, lexer);
            if (lexer->lookahead == '|') advance(s, lexer);
            continue;
        }
        if (is_newline(c)) {
            if (single_line) return false;
            consume_newline(s, lexer);
            while (is_blank(lexer->lookahead)) advance(s, lexer);
            if (is_newline(lexer->lookahead) || lexer->eof(lexer)) return false; // blank line
            if (!line_certainly_continues(s, lexer)) return true; // cannot tell
            continue;
        }
        advance(s, lexer);
    }
}

// Emphasis, strong emphasis and strikethrough delimiters (upstream
// `parse_star` / `parse_underscore` / `parse_tilde` of the inline scanner),
// once the first delimiter character has been consumed and the run counted.
// OKF: was the character before a delimiter run at `column` whitespace (or
// the start of the line) / punctuation?  Upstream reads this only from the
// validity of the `_last_token_*` markers, which is lost when an incremental
// re-parse reuses the subtree ending just before the run: the markers are
// only valid in the parse state right after the preceding token is shifted.
// A marker is never falsely valid, so either source is trusted.
static void prev_char_class(Scanner *s, const bool *valid_symbols, uint32_t column,
                            bool *whitespace, bool *punctuation) {
    bool recorded = s->prev_column == column;
    *whitespace = valid_symbols[LAST_TOKEN_WHITESPACE] || column == 0 ||
                  (recorded && s->prev_class == CLASS_WHITESPACE);
    *punctuation = !*whitespace &&
                   (valid_symbols[LAST_TOKEN_PUNCTUATION] ||
                    (recorded && s->prev_class == CLASS_PUNCTUATION));
}

// A delimiter character that is neither an opener nor a closer is literal
// text.  Usually the internal lexer takes it (return false); but when another
// delimiter follows it (the rest of its run, or a different run as in `~_`),
// it is emitted here as its punctuation token so the scanner state records it
// (delta I2).  Only the first character is emitted: the token ends where
// `mark_end` was left, right after it.
// With `whole_run`, the rest of the run is literal too (STATE_LITERAL_RUN).
static bool emit_literal_delimiter(Scanner *s, TSLexer *lexer, const bool *valid_symbols,
                                   int32_t delimiter, uint8_t run, uint32_t column,
                                   bool next_is_delimiter, bool whole_run) {
    int token = punctuation_token(delimiter);
    bool followed = run > 1 || next_is_delimiter;
    if (s->simulate || !followed || token < 0 || !valid_symbols[token]) return false;
    s->inline_state &= ~(STATE_EMPHASIS_DELIMITER_IS_OPEN | STATE_LITERAL_RUN);
    if (whole_run && run > 1) {
        s->num_emphasis_delimiters_left = run - 1;
        s->inline_state |= STATE_LITERAL_RUN;
    } else {
        s->num_emphasis_delimiters_left = 0;
    }
    note_prev(s, CLASS_PUNCTUATION, column + 1);
    lexer->result_symbol = (TSSymbol)token;
    return true;
}

static bool inline_emphasis(Scanner *s, TSLexer *lexer,
                            const bool *valid_symbols, uint8_t run,
                            TokenType open, TokenType close, uint32_t column) {
    bool line_end = is_newline(lexer->lookahead) || lexer->eof(lexer);
    if (!(valid_symbols[open] || valid_symbols[close])) return false;
    // (read before any lookahead scan moves the lexer on)
    bool next_is_delimiter = is_emphasis_delimiter(lexer->lookahead);
    bool last_whitespace, last_punctuation;
    prev_char_class(s, valid_symbols, column, &last_whitespace, &last_punctuation);
    note_prev(s, CLASS_PUNCTUATION, column + 1);
    // The decision made for the first delimiter also counts for all the
    // following delimiters in the run.  Remember how many there are.
    s->num_emphasis_delimiters_left = run - 1;
    // Look ahead to the next symbol (after the last delimiter) to find out if
    // it is whitespace, punctuation or other.
    bool next_symbol_whitespace = line_end || is_blank(lexer->lookahead);
    bool next_symbol_punctuation = is_punctuation(lexer->lookahead);
    // Information about the last token is in valid_symbols.  See grammar.js
    // for these tokens for how this is done.
    if (valid_symbols[close] && !last_whitespace &&
        (!last_punctuation || next_symbol_punctuation ||
         next_symbol_whitespace)) {
        // Closing delimiters take precedence
        s->inline_state &= ~STATE_EMPHASIS_DELIMITER_IS_OPEN;
        lexer->result_symbol = close;
        return true;
    }
    if (!next_symbol_whitespace &&
        (!next_symbol_punctuation || last_punctuation || last_whitespace)) {
        // OKF delta I3: a run no delimiter of its kind follows in this
        // paragraph opens nothing; leaving it to the lexer as text keeps the
        // parser from forking on it (upstream forks on every opener, which
        // can exhaust tree-sitter's parse versions).
        int32_t delimiter = open == STRIKETHROUGH_OPEN ? '~'
                          : open == EMPHASIS_OPEN_STAR ? '*' : '_';
        if (!s->simulate && !closer_possible(s, lexer, delimiter)) {
            // nothing after the run closes it, so nothing in it opens either
            return emit_literal_delimiter(s, lexer, valid_symbols, delimiter, run, column,
                                          next_is_delimiter, true);
        }
        s->inline_state |= STATE_EMPHASIS_DELIMITER_IS_OPEN;
        lexer->result_symbol = open;
        return true;
    }
    int32_t delimiter = open == STRIKETHROUGH_OPEN ? '~' : open == EMPHASIS_OPEN_STAR ? '*' : '_';
    return emit_literal_delimiter(s, lexer, valid_symbols, delimiter, run, column,
                                  next_is_delimiter, false);
}

// The rest of a delimiter run whose first character already decided open or
// close (upstream fast path).
static bool inline_emphasis_continue(Scanner *s, TSLexer *lexer,
                                     const bool *valid_symbols, TokenType open,
                                     TokenType close, uint32_t column) {
    if (s->inline_state & STATE_LITERAL_RUN) {
        int32_t delimiter = open == STRIKETHROUGH_OPEN ? '~'
                          : open == EMPHASIS_OPEN_STAR ? '*' : '_';
        int token = punctuation_token(delimiter);
        if (s->simulate || token < 0 || !valid_symbols[token]) {
            // cannot continue here: the run is decided afresh
            s->num_emphasis_delimiters_left = 0;
            s->inline_state &= ~STATE_LITERAL_RUN;
            return false;
        }
        note_prev(s, CLASS_PUNCTUATION, column + 1);
        if (--s->num_emphasis_delimiters_left == 0) s->inline_state &= ~STATE_LITERAL_RUN;
        lexer->result_symbol = (TSSymbol)token;
        return true;
    }
    note_prev(s, CLASS_PUNCTUATION, column + 1);
    if ((s->inline_state & STATE_EMPHASIS_DELIMITER_IS_OPEN) &&
        valid_symbols[open]) {
        s->inline_state &= (~STATE_EMPHASIS_DELIMITER_IS_OPEN);
        lexer->result_symbol = open;
        s->num_emphasis_delimiters_left--;
        return true;
    }
    if (valid_symbols[close]) {
        lexer->result_symbol = close;
        s->num_emphasis_delimiters_left--;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------------
 * Merged block/inline decisions
 * ------------------------------------------------------------------------ */

// '`': a fenced code block delimiter (block) or a code span delimiter (inline).
static bool parse_backtick(Scanner *s, TSLexer *lexer, const bool *valid_symbols,
                           bool consumed_ws) {
    bool block_ok = valid_symbols[FENCED_CODE_BLOCK_START_BACKTICK] ||
                    valid_symbols[FENCED_CODE_BLOCK_END_BACKTICK];
    bool inline_ok = valid_symbols[CODE_SPAN_START] ||
                     valid_symbols[CODE_SPAN_CLOSE] ||
                     valid_symbols[UNCLOSED_SPAN];
    if (!block_ok && !inline_ok) return false;

    // count the number of backticks
    uint8_t level = 0;
    while (lexer->lookahead == '`') {
        advance(s, lexer);
        level++;
    }
    mark_end(s, lexer);
    // If this is able to close a fenced code block then that is the only valid
    // interpretation.  It can only close a fenced code block if the number of
    // backticks is at least the number of backticks of the opening delimiter.
    // Also it cannot be indented more than 3 spaces.
    if (valid_symbols[FENCED_CODE_BLOCK_END_BACKTICK] && s->indentation < 4 &&
        level >= s->fenced_code_block_delimiter_length) {
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            advance(s, lexer);
        }
        if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
            s->fenced_code_block_delimiter_length = 0;
            lexer->result_symbol = FENCED_CODE_BLOCK_END_BACKTICK;
            return true;
        }
    }
    // If this could be the start of a fenced code block, check if the info
    // string contains any backticks.
    if (valid_symbols[FENCED_CODE_BLOCK_START_BACKTICK] && level >= 3) {
        bool info_string_has_backtick = false;
        while (lexer->lookahead != '\n' && lexer->lookahead != '\r' &&
               !lexer->eof(lexer)) {
            if (lexer->lookahead == '`') {
                info_string_has_backtick = true;
                break;
            }
            advance(s, lexer);
        }
        if (!info_string_has_backtick) {
            lexer->result_symbol = FENCED_CODE_BLOCK_START_BACKTICK;
            if (!s->simulate) push_block(s, FENCED_CODE_BLOCK);
            // Remember the length of the delimiter for later, since we need it
            // to decide whether a sequence of backticks can close the block.
            s->fenced_code_block_delimiter_length = level;
            s->indentation = 0;
            return true;
        }
        // OKF: otherwise this is inline content; the closer search below
        // continues from the backtick in the info string, having seen no
        // backtick in between.
    }

    // OKF: inline code spans.  Never after whitespace this call consumed: that
    // whitespace has to be an inline token of its own first.
    if (consumed_ws || !inline_ok) return false;
    if (level == s->code_span_delimiter_length &&
        valid_symbols[CODE_SPAN_CLOSE]) {
        s->code_span_delimiter_length = 0;
        if (is_emphasis_delimiter(lexer->lookahead)) {
            note_prev(s, CLASS_PUNCTUATION, lexer->get_column(lexer));
        }
        lexer->result_symbol = CODE_SPAN_CLOSE;
        return true;
    }
    if (valid_symbols[CODE_SPAN_START]) {
        if (code_span_closer_ahead(s, lexer, level)) {
            s->code_span_delimiter_length = level;
            lexer->result_symbol = CODE_SPAN_START;
            return true;
        }
        if (valid_symbols[UNCLOSED_SPAN]) {
            lexer->result_symbol = UNCLOSED_SPAN;
            return true;
        }
    }
    return false;
}

// '~': a fenced code block delimiter (block) or strikethrough (inline).
static bool parse_tilde(Scanner *s, TSLexer *lexer, const bool *valid_symbols,
                        bool consumed_ws) {
    bool block_ok = valid_symbols[FENCED_CODE_BLOCK_START_TILDE] ||
                    valid_symbols[FENCED_CODE_BLOCK_END_TILDE];
    bool inline_ok = valid_symbols[STRIKETHROUGH_OPEN] ||
                     valid_symbols[STRIKETHROUGH_CLOSE];
    if (!block_ok && !inline_ok) return false;

    uint32_t column = inline_ok && !consumed_ws && !s->simulate ? lexer->get_column(lexer) : 0;
    advance(s, lexer);
    if (!block_ok && !consumed_ws && s->num_emphasis_delimiters_left > 0 &&
        inline_emphasis_continue(s, lexer, valid_symbols, STRIKETHROUGH_OPEN,
                                 STRIKETHROUGH_CLOSE, column)) {
        return true;
    }
    if (!s->leading_ws_column) mark_end(s, lexer);
    uint8_t level = 1;
    while (lexer->lookahead == '~') {
        advance(s, lexer);
        level++;
    }
    if (block_ok) {
        if (valid_symbols[FENCED_CODE_BLOCK_END_TILDE] && s->indentation < 4 &&
            level >= s->fenced_code_block_delimiter_length) {
            mark_end(s, lexer);
            while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                advance(s, lexer);
            }
            if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
                s->fenced_code_block_delimiter_length = 0;
                lexer->result_symbol = FENCED_CODE_BLOCK_END_TILDE;
                return true;
            }
            return false;
        }
        if (valid_symbols[FENCED_CODE_BLOCK_START_TILDE] && level >= 3) {
            mark_end(s, lexer);
            lexer->result_symbol = FENCED_CODE_BLOCK_START_TILDE;
            if (!s->simulate) push_block(s, FENCED_CODE_BLOCK);
            s->fenced_code_block_delimiter_length = level;
            s->indentation = 0;
            return true;
        }
    }
    if (consumed_ws) {
        if (s->leading_ws_column) {
            return emit_leading_whitespace(s, lexer, valid_symbols, s->leading_ws_column,
                                           s->leading_ws_single);
        }
        return false;
    }
    return inline_emphasis(s, lexer, valid_symbols, level, STRIKETHROUGH_OPEN,
                           STRIKETHROUGH_CLOSE, column);
}

// '*': a list marker or thematic break (block) or emphasis (inline).
static bool parse_star(Scanner *s, TSLexer *lexer, const bool *valid_symbols,
                       bool consumed_ws) {
    bool block_ok = valid_symbols[THEMATIC_BREAK] ||
                    valid_symbols[LIST_MARKER_STAR] ||
                    valid_symbols[LIST_MARKER_STAR_DONT_INTERRUPT];
    bool inline_ok = valid_symbols[EMPHASIS_OPEN_STAR] ||
                     valid_symbols[EMPHASIS_CLOSE_STAR];
    if (!block_ok && !inline_ok) return false;

    uint32_t column = inline_ok && !consumed_ws && !s->simulate ? lexer->get_column(lexer) : 0;
    advance(s, lexer);
    if (!block_ok && !consumed_ws && s->num_emphasis_delimiters_left > 0 &&
        inline_emphasis_continue(s, lexer, valid_symbols, EMPHASIS_OPEN_STAR,
                                 EMPHASIS_CLOSE_STAR, column)) {
        return true;
    }
    if (!s->leading_ws_column) mark_end(s, lexer);
    uint8_t run = 1;
    while (lexer->lookahead == '*') {
        run++;
        advance(s, lexer);
    }
    bool next_ws = is_blank(lexer->lookahead) || is_newline(lexer->lookahead) ||
                   lexer->eof(lexer);

    if (block_ok && next_ws) {
        // Upstream block logic: count the stars permitting whitespace between
        // them, and remember how many spaces follow the first star.
        size_t star_count = run;
        uint8_t extra_indentation = 0;
        for (;;) {
            if (lexer->lookahead == '*') {
                if (star_count == 1 && extra_indentation >= 1 &&
                    valid_symbols[LIST_MARKER_STAR]) {
                    // If we get to this point then the token has to be at
                    // least this long.  `mark_end` is needed here in case this
                    // turns out to be a list item.
                    mark_end(s, lexer);
                }
                star_count++;
                advance(s, lexer);
            } else if (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                if (star_count == 1) {
                    extra_indentation += advance(s, lexer);
                } else {
                    advance(s, lexer);
                }
            } else {
                break;
            }
        }
        bool line_end = lexer->lookahead == '\n' || lexer->lookahead == '\r';
        bool dont_interrupt = false;
        if (star_count == 1 && line_end) {
            extra_indentation = 1;
            // line is empty so don't interrupt paragraphs if this is a list
            // marker
            dont_interrupt = s->matched == s->open_blocks.size;
        }
        // If there were at least 3 stars then this could be a thematic break
        bool thematic_break = star_count >= 3 && line_end;
        // If there was a star and at least one space after that star then this
        // could be a list marker.
        bool list_marker_star = star_count >= 1 && extra_indentation >= 1;
        if (valid_symbols[THEMATIC_BREAK] && thematic_break &&
            s->indentation < 4) {
            // If a thematic break is valid then it takes precedence
            lexer->result_symbol = THEMATIC_BREAK;
            mark_end(s, lexer);
            s->indentation = 0;
            return true;
        }
        if ((dont_interrupt ? valid_symbols[LIST_MARKER_STAR_DONT_INTERRUPT]
                            : valid_symbols[LIST_MARKER_STAR]) &&
            list_marker_star) {
            // List markers take precedence over emphasis markers.  If
            // star_count > 1 then `mark_end` was already called at the right
            // point.  Otherwise the token should go until this point.
            if (star_count == 1) {
                mark_end(s, lexer);
            }
            // Not counting one space...
            extra_indentation--;
            // ... check if the list item begins with an indented code block
            if (extra_indentation <= 3) {
                // If not then the indentation level of the list item content is
                // the indentation of the list marker + the indentation after
                // the list marker - 1
                extra_indentation += s->indentation;
                s->indentation = 0;
            } else {
                // Otherwise the indentation level is just the indentation of
                // the list marker.  The indentation after the list marker is
                // kept for later blocks.
                uint8_t temp = s->indentation;
                s->indentation = extra_indentation;
                extra_indentation = temp;
            }
            if (!s->simulate) push_block(s, (Block)(LIST_ITEM + extra_indentation));
            lexer->result_symbol = dont_interrupt ? LIST_MARKER_STAR_DONT_INTERRUPT
                                                  : LIST_MARKER_STAR;
            return true;
        }
        return false;
    }
    if (consumed_ws) {
        if (s->leading_ws_column) {
            return emit_leading_whitespace(s, lexer, valid_symbols, s->leading_ws_column,
                                           s->leading_ws_single);
        }
        return false;
    }
    return inline_emphasis(s, lexer, valid_symbols, run, EMPHASIS_OPEN_STAR,
                           EMPHASIS_CLOSE_STAR, column);
}

// '_': a thematic break (block) or emphasis (inline).
static bool parse_underscore(Scanner *s, TSLexer *lexer,
                             const bool *valid_symbols, bool consumed_ws) {
    bool block_ok = valid_symbols[THEMATIC_BREAK];
    bool inline_ok = valid_symbols[EMPHASIS_OPEN_UNDERSCORE] ||
                     valid_symbols[EMPHASIS_CLOSE_UNDERSCORE];
    if (!block_ok && !inline_ok) return false;

    uint32_t column = inline_ok && !consumed_ws && !s->simulate ? lexer->get_column(lexer) : 0;
    advance(s, lexer);
    if (!block_ok && !consumed_ws && s->num_emphasis_delimiters_left > 0 &&
        inline_emphasis_continue(s, lexer, valid_symbols,
                                 EMPHASIS_OPEN_UNDERSCORE,
                                 EMPHASIS_CLOSE_UNDERSCORE, column)) {
        return true;
    }
    if (!s->leading_ws_column) mark_end(s, lexer);
    uint8_t run = 1;
    while (lexer->lookahead == '_') {
        run++;
        advance(s, lexer);
    }
    bool next_ws = is_blank(lexer->lookahead) || is_newline(lexer->lookahead) ||
                   lexer->eof(lexer);
    if (block_ok && next_ws) {
        size_t underscore_count = run;
        for (;;) {
            if (lexer->lookahead == '_') {
                underscore_count++;
                advance(s, lexer);
            } else if (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                advance(s, lexer);
            } else {
                break;
            }
        }
        bool line_end = lexer->lookahead == '\n' || lexer->lookahead == '\r';
        if (underscore_count >= 3 && line_end && s->indentation < 4) {
            lexer->result_symbol = THEMATIC_BREAK;
            mark_end(s, lexer);
            s->indentation = 0;
            return true;
        }
        return false;
    }
    if (consumed_ws) {
        if (s->leading_ws_column) {
            return emit_leading_whitespace(s, lexer, valid_symbols, s->leading_ws_column,
                                           s->leading_ws_single);
        }
        return false;
    }
    return inline_emphasis(s, lexer, valid_symbols, run, EMPHASIS_OPEN_UNDERSCORE,
                           EMPHASIS_CLOSE_UNDERSCORE, column);
}

/* ------------------------------------------------------------------------
 * Block starts (upstream)
 * ------------------------------------------------------------------------ */

static bool parse_block_quote(Scanner *s, TSLexer *lexer,
                              const bool *valid_symbols) {
    if (valid_symbols[BLOCK_QUOTE_START]) {
        advance(s, lexer);
        s->indentation = 0;
        if (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            s->indentation += advance(s, lexer) - 1;
        }
        lexer->result_symbol = BLOCK_QUOTE_START;
        if (!s->simulate) push_block(s, BLOCK_QUOTE);
        return true;
    }
    return false;
}

static bool parse_atx_heading(Scanner *s, TSLexer *lexer,
                              const bool *valid_symbols) {
    if (valid_symbols[ATX_H1_MARKER] && s->indentation <= 3) {
        mark_end(s, lexer);
        uint16_t level = 0;
        while (lexer->lookahead == '#' && level <= 6) {
            advance(s, lexer);
            level++;
        }
        if (level <= 6 &&
            (lexer->lookahead == ' ' || lexer->lookahead == '\t' ||
             lexer->lookahead == '\n' || lexer->lookahead == '\r' ||
             lexer->eof(lexer))) {
            lexer->result_symbol = ATX_H1_MARKER + (level - 1);
            s->indentation = 0;
            // OKF: the heading is one line; its ending is never soft.
            if (!s->simulate) s->state |= STATE_SINGLE_LINE;
            mark_end(s, lexer);
            return true;
        }
        if (level == 1) return emit_lone_punctuation(s, lexer, valid_symbols, '#');
    }
    return false;
}

static bool parse_setext_underline(Scanner *s, TSLexer *lexer,
                                   const bool *valid_symbols) {
    if (valid_symbols[SETEXT_H1_UNDERLINE] &&
        s->matched == s->open_blocks.size) {
        mark_end(s, lexer);
        while (lexer->lookahead == '=') {
            advance(s, lexer);
        }
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            advance(s, lexer);
        }
        if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
            lexer->result_symbol = SETEXT_H1_UNDERLINE;
            mark_end(s, lexer);
            return true;
        }
    }
    return false;
}

static bool parse_plus(Scanner *s, TSLexer *lexer, const bool *valid_symbols) {
    // OKF: `+++` metadata is not part of the profile (spec D7).
    if (s->indentation <= 3 &&
        (valid_symbols[LIST_MARKER_PLUS] ||
         valid_symbols[LIST_MARKER_PLUS_DONT_INTERRUPT])) {
        advance(s, lexer);
        uint8_t extra_indentation = 0;
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            extra_indentation += advance(s, lexer);
        }
        bool dont_interrupt = false;
        if (lexer->lookahead == '\r' || lexer->lookahead == '\n') {
            extra_indentation = 1;
            dont_interrupt = true;
        }
        dont_interrupt = dont_interrupt && s->matched == s->open_blocks.size;
        if (extra_indentation >= 1 &&
            (dont_interrupt ? valid_symbols[LIST_MARKER_PLUS_DONT_INTERRUPT]
                            : valid_symbols[LIST_MARKER_PLUS])) {
            lexer->result_symbol =
                dont_interrupt ? LIST_MARKER_PLUS_DONT_INTERRUPT : LIST_MARKER_PLUS;
            extra_indentation--;
            if (extra_indentation <= 3) {
                extra_indentation += s->indentation;
                s->indentation = 0;
            } else {
                uint8_t temp = s->indentation;
                s->indentation = extra_indentation;
                extra_indentation = temp;
            }
            if (!s->simulate) push_block(s, (Block)(LIST_ITEM + extra_indentation));
            return true;
        }
        if (extra_indentation == 0) {
            return emit_lone_punctuation(s, lexer, valid_symbols, '+');
        }
    }
    return false;
}

static bool parse_ordered_list_marker(Scanner *s, TSLexer *lexer,
                                      const bool *valid_symbols) {
    if (s->indentation <= 3 &&
        (valid_symbols[LIST_MARKER_PARENTHESIS] ||
         valid_symbols[LIST_MARKER_DOT] ||
         valid_symbols[LIST_MARKER_PARENTHESIS_DONT_INTERRUPT] ||
         valid_symbols[LIST_MARKER_DOT_DONT_INTERRUPT])) {
        size_t digits = 1;
        bool dont_interrupt = !isdigit(lexer->lookahead);
        advance(s, lexer);
        while (isdigit(lexer->lookahead)) {
            dont_interrupt = true;
            digits++;
            advance(s, lexer);
        }
        if (digits >= 1 && digits <= 9) {
            bool dot = false;
            bool parenthesis = false;
            if (lexer->lookahead == '.') {
                advance(s, lexer);
                dot = true;
            } else if (lexer->lookahead == ')') {
                advance(s, lexer);
                parenthesis = true;
            }
            if (dot || parenthesis) {
                uint8_t extra_indentation = 0;
                while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                    extra_indentation += advance(s, lexer);
                }
                bool line_end =
                    lexer->lookahead == '\n' || lexer->lookahead == '\r';
                if (line_end) {
                    extra_indentation = 1;
                    dont_interrupt = true;
                }
                dont_interrupt =
                    dont_interrupt && s->matched == s->open_blocks.size;
                if (extra_indentation >= 1 &&
                    (dot ? (dont_interrupt
                                ? valid_symbols[LIST_MARKER_DOT_DONT_INTERRUPT]
                                : valid_symbols[LIST_MARKER_DOT])
                         : (dont_interrupt
                                ? valid_symbols[LIST_MARKER_PARENTHESIS_DONT_INTERRUPT]
                                : valid_symbols[LIST_MARKER_PARENTHESIS]))) {
                    lexer->result_symbol =
                        dot ? LIST_MARKER_DOT : LIST_MARKER_PARENTHESIS;
                    extra_indentation--;
                    if (extra_indentation <= 3) {
                        extra_indentation += s->indentation;
                        s->indentation = 0;
                    } else {
                        uint8_t temp = s->indentation;
                        s->indentation = extra_indentation;
                        extra_indentation = temp;
                    }
                    if (!s->simulate)
                        push_block(s, (Block)(LIST_ITEM + extra_indentation + digits));
                    return true;
                }
            }
        }
    }
    return false;
}

static void fm_reset(Scanner *s);

static bool parse_minus(Scanner *s, TSLexer *lexer, const bool *valid_symbols) {
    if (s->indentation <= 3 &&
        (valid_symbols[LIST_MARKER_MINUS] ||
         valid_symbols[LIST_MARKER_MINUS_DONT_INTERRUPT] ||
         valid_symbols[SETEXT_H2_UNDERLINE] || valid_symbols[THEMATIC_BREAK] ||
         valid_symbols[FM_OPEN])) {
        mark_end(s, lexer);
        bool whitespace_after_minus = false;
        bool minus_after_whitespace = false;
        size_t minus_count = 0;
        uint8_t extra_indentation = 0;

        for (;;) {
            if (lexer->lookahead == '-') {
                if (minus_count == 1 && extra_indentation >= 1) {
                    mark_end(s, lexer);
                }
                minus_count++;
                advance(s, lexer);
                minus_after_whitespace = whitespace_after_minus;
            } else if (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                if (minus_count == 1) {
                    extra_indentation += advance(s, lexer);
                } else {
                    advance(s, lexer);
                }
                whitespace_after_minus = true;
            } else {
                break;
            }
        }
        bool line_end = lexer->lookahead == '\n' || lexer->lookahead == '\r';

        // OKF: the frontmatter opener.  `---` then a line ending at byte
        // offset 0, and nothing else: `--- ` and `----` are not delimiters
        // (spec §4.2).  Only valid in the initial parse state.
        if (valid_symbols[FM_OPEN] && !s->simulate && minus_count == 3 &&
            !whitespace_after_minus && line_end && s->indentation == 0) {
            consume_newline(s, lexer);
            lexer->mark_end(lexer);
            fm_reset(s);
            s->fm_active = true;
            s->fm_line = FM_LINE_START;
            s->indentation = 0;
            s->column = 0;
            lexer->result_symbol = FM_OPEN;
            return true;
        }

        bool dont_interrupt = false;
        if (minus_count == 1 && line_end) {
            extra_indentation = 1;
            dont_interrupt = true;
        }
        dont_interrupt = dont_interrupt && s->matched == s->open_blocks.size;
        bool thematic_break = minus_count >= 3 && line_end;
        bool underline =
            minus_count >= 1 && !minus_after_whitespace && line_end &&
            s->matched == s->open_blocks.size; // setext heading can not break lazy continuation
        bool list_marker_minus = minus_count >= 1 && extra_indentation >= 1;
        if (valid_symbols[SETEXT_H2_UNDERLINE] && underline) {
            lexer->result_symbol = SETEXT_H2_UNDERLINE;
            mark_end(s, lexer);
            s->indentation = 0;
            return true;
        }
        if (valid_symbols[THEMATIC_BREAK] && thematic_break) {
            // underline is false if list_marker_minus is true
            lexer->result_symbol = THEMATIC_BREAK;
            mark_end(s, lexer);
            s->indentation = 0;
            return true;
        }
        if ((dont_interrupt ? valid_symbols[LIST_MARKER_MINUS_DONT_INTERRUPT]
                            : valid_symbols[LIST_MARKER_MINUS]) &&
            list_marker_minus) {
            if (minus_count == 1) {
                mark_end(s, lexer);
            }
            extra_indentation--;
            if (extra_indentation <= 3) {
                extra_indentation += s->indentation;
                s->indentation = 0;
            } else {
                uint8_t temp = s->indentation;
                s->indentation = extra_indentation;
                extra_indentation = temp;
            }
            if (!s->simulate) push_block(s, (Block)(LIST_ITEM + extra_indentation));
            lexer->result_symbol =
                dont_interrupt ? LIST_MARKER_MINUS_DONT_INTERRUPT : LIST_MARKER_MINUS;
            return true;
        }
        if (minus_count == 1 && !whitespace_after_minus) {
            return emit_lone_punctuation(s, lexer, valid_symbols, '-');
        }
    }
    return false;
}

static bool parse_html_block(Scanner *s, TSLexer *lexer,
                             const bool *valid_symbols) {
    if (!(valid_symbols[HTML_BLOCK_1_START] || valid_symbols[HTML_BLOCK_1_END] ||
          valid_symbols[HTML_BLOCK_2_START] || valid_symbols[HTML_BLOCK_3_START] ||
          valid_symbols[HTML_BLOCK_4_START] || valid_symbols[HTML_BLOCK_5_START] ||
          valid_symbols[HTML_BLOCK_6_START] || valid_symbols[HTML_BLOCK_7_START])) {
        return false;
    }
    advance(s, lexer);
    if (lexer->lookahead == '?' && valid_symbols[HTML_BLOCK_3_START]) {
        advance(s, lexer);
        lexer->result_symbol = HTML_BLOCK_3_START;
        if (!s->simulate) push_block(s, ANONYMOUS);
        return true;
    }
    if (lexer->lookahead == '!') {
        // could be block 2
        advance(s, lexer);
        if (lexer->lookahead == '-') {
            advance(s, lexer);
            if (lexer->lookahead == '-' && valid_symbols[HTML_BLOCK_2_START]) {
                advance(s, lexer);
                lexer->result_symbol = HTML_BLOCK_2_START;
                if (!s->simulate) push_block(s, ANONYMOUS);
                return true;
            }
        } else if ('A' <= lexer->lookahead && lexer->lookahead <= 'Z' &&
                   valid_symbols[HTML_BLOCK_4_START]) {
            advance(s, lexer);
            lexer->result_symbol = HTML_BLOCK_4_START;
            if (!s->simulate) push_block(s, ANONYMOUS);
            return true;
        } else if (lexer->lookahead == '[') {
            advance(s, lexer);
            if (lexer->lookahead == 'C') {
                advance(s, lexer);
                if (lexer->lookahead == 'D') {
                    advance(s, lexer);
                    if (lexer->lookahead == 'A') {
                        advance(s, lexer);
                        if (lexer->lookahead == 'T') {
                            advance(s, lexer);
                            if (lexer->lookahead == 'A') {
                                advance(s, lexer);
                                if (lexer->lookahead == '[' &&
                                    valid_symbols[HTML_BLOCK_5_START]) {
                                    advance(s, lexer);
                                    lexer->result_symbol = HTML_BLOCK_5_START;
                                    if (!s->simulate) push_block(s, ANONYMOUS);
                                    return true;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    bool starting_slash = lexer->lookahead == '/';
    if (starting_slash) {
        advance(s, lexer);
    }
    char name[11];
    size_t name_length = 0;
    while (iswalpha((wint_t)lexer->lookahead)) {
        if (name_length < 10) {
            name[name_length++] = (char)towlower((wint_t)lexer->lookahead);
        } else {
            name_length = 12;
        }
        advance(s, lexer);
    }
    if (name_length == 0) {
        if (!starting_slash) return emit_lone_punctuation(s, lexer, valid_symbols, '<');
        return false;
    }
    bool tag_closed = false;
    if (name_length < 11) {
        name[name_length] = 0;
        bool next_symbol_valid =
            lexer->lookahead == ' ' || lexer->lookahead == '\t' ||
            lexer->lookahead == '\n' || lexer->lookahead == '\r' ||
            lexer->lookahead == '>';
        if (next_symbol_valid) {
            // try block 1 names
            for (size_t i = 0; i < NUM_HTML_TAG_NAMES_RULE_1; i++) {
                if (strcmp(name, HTML_TAG_NAMES_RULE_1[i]) == 0) {
                    if (starting_slash) {
                        if (valid_symbols[HTML_BLOCK_1_END]) {
                            lexer->result_symbol = HTML_BLOCK_1_END;
                            return true;
                        }
                    } else if (valid_symbols[HTML_BLOCK_1_START]) {
                        lexer->result_symbol = HTML_BLOCK_1_START;
                        if (!s->simulate) push_block(s, ANONYMOUS);
                        return true;
                    }
                }
            }
        }
        if (!next_symbol_valid && lexer->lookahead == '/') {
            advance(s, lexer);
            if (lexer->lookahead == '>') {
                advance(s, lexer);
                tag_closed = true;
            }
        }
        if (next_symbol_valid || tag_closed) {
            // try block 6 names
            for (size_t i = 0; i < NUM_HTML_TAG_NAMES_RULE_7; i++) {
                if (strcmp(name, HTML_TAG_NAMES_RULE_7[i]) == 0 &&
                    valid_symbols[HTML_BLOCK_6_START]) {
                    lexer->result_symbol = HTML_BLOCK_6_START;
                    if (!s->simulate) push_block(s, ANONYMOUS);
                    return true;
                }
            }
        }
    }

    if (!valid_symbols[HTML_BLOCK_7_START]) {
        return false;
    }

    if (!tag_closed) {
        // tag name (continued)
        while (iswalnum((wint_t)lexer->lookahead) || lexer->lookahead == '-') {
            advance(s, lexer);
        }
        if (!starting_slash) {
            // attributes
            bool had_whitespace = false;
            for (;;) {
                // whitespace
                while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                    had_whitespace = true;
                    advance(s, lexer);
                }
                if (lexer->lookahead == '/') {
                    advance(s, lexer);
                    break;
                }
                if (lexer->lookahead == '>') {
                    break;
                }
                // attribute name
                if (!had_whitespace) {
                    return false;
                }
                if (!iswalpha((wint_t)lexer->lookahead) &&
                    lexer->lookahead != '_' && lexer->lookahead != ':') {
                    return false;
                }
                had_whitespace = false;
                advance(s, lexer);
                while (iswalnum((wint_t)lexer->lookahead) ||
                       lexer->lookahead == '_' || lexer->lookahead == '.' ||
                       lexer->lookahead == ':' || lexer->lookahead == '-') {
                    advance(s, lexer);
                }
                // attribute value specification
                // optional whitespace
                while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                    had_whitespace = true;
                    advance(s, lexer);
                }
                // =
                if (lexer->lookahead == '=') {
                    advance(s, lexer);
                    had_whitespace = false;
                    // optional whitespace
                    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                        advance(s, lexer);
                    }
                    // attribute value
                    if (lexer->lookahead == '\'' || lexer->lookahead == '"') {
                        char delimiter = (char)lexer->lookahead;
                        advance(s, lexer);
                        while (lexer->lookahead != delimiter &&
                               lexer->lookahead != '\n' &&
                               lexer->lookahead != '\r' && !lexer->eof(lexer)) {
                            advance(s, lexer);
                        }
                        if (lexer->lookahead != delimiter) {
                            return false;
                        }
                        advance(s, lexer);
                    } else {
                        // unquoted attribute value
                        bool had_one = false;
                        while (lexer->lookahead != ' ' &&
                               lexer->lookahead != '\t' &&
                               lexer->lookahead != '"' &&
                               lexer->lookahead != '\'' &&
                               lexer->lookahead != '=' &&
                               lexer->lookahead != '<' &&
                               lexer->lookahead != '>' &&
                               lexer->lookahead != '`' &&
                               lexer->lookahead != '\n' &&
                               lexer->lookahead != '\r' && !lexer->eof(lexer)) {
                            advance(s, lexer);
                            had_one = true;
                        }
                        if (!had_one) {
                            return false;
                        }
                    }
                }
            }
        } else {
            while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                advance(s, lexer);
            }
        }
        if (lexer->lookahead != '>') {
            return false;
        }
        advance(s, lexer);
    }
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
        advance(s, lexer);
    }
    if (lexer->lookahead == '\r' || lexer->lookahead == '\n') {
        lexer->result_symbol = HTML_BLOCK_7_START;
        if (!s->simulate) push_block(s, ANONYMOUS);
        return true;
    }
    return false;
}

// The rest of a pipe table start check, from wherever the first line has been
// read up to (upstream `parse_pipe_table`, split so the `[` path can continue
// it after looking for a footnote).  `mark_end` must already be at the start:
// PIPE_TABLE_START is zero width.
static bool parse_pipe_table_rest(Scanner *s, TSLexer *lexer, bool starting_pipe,
                                  bool *saw_close_bracket, bool *crossed_line) {
    // count number of cells
    size_t cell_count = 0;
    // also remember if we see starting and ending pipes, as empty headers have
    // to have both
    bool ending_pipe = false;
    bool empty = true;
    while (lexer->lookahead != '\r' && lexer->lookahead != '\n' &&
           !lexer->eof(lexer)) {
        if (lexer->lookahead == ']' && saw_close_bracket) *saw_close_bracket = true;
        if (lexer->lookahead == '|') {
            cell_count++;
            ending_pipe = true;
            advance(s, lexer);
        } else {
            if (lexer->lookahead != ' ' && lexer->lookahead != '\t') {
                ending_pipe = false;
            }
            if (lexer->lookahead == '\\') {
                advance(s, lexer);
                if (is_punctuation(lexer->lookahead)) {
                    advance(s, lexer);
                }
            } else {
                advance(s, lexer);
            }
        }
    }
    if (empty && cell_count == 0 && !(starting_pipe && ending_pipe)) {
        return false;
    }
    if (!ending_pipe) {
        cell_count++;
    }

    // check the following line for a delimiter row
    // parse a newline
    if (crossed_line) *crossed_line = true;
    if (lexer->lookahead == '\n') {
        advance(s, lexer);
    } else if (lexer->lookahead == '\r') {
        advance(s, lexer);
        if (lexer->lookahead == '\n') {
            advance(s, lexer);
        }
    } else {
        return false;
    }
    s->indentation = 0;
    s->column = 0;
    for (;;) {
        if (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            s->indentation += advance(s, lexer);
        } else {
            break;
        }
    }
    s->simulate = true;
    uint8_t matched_temp = 0;
    while (matched_temp < (uint8_t)s->open_blocks.size) {
        if (match(s, lexer, s->open_blocks.items[matched_temp])) {
            matched_temp++;
        } else {
            return false;
        }
    }

    // check if delimiter row has the same number of cells and at least one pipe
    // OKF: every delimiter cell must contain a `-` (GFM); upstream counted
    // empty cells too, so `||` passed as a delimiter row the grammar rejects.
    size_t delimiter_cell_count = 0;
    if (lexer->lookahead == '|') {
        advance(s, lexer);
    }
    for (;;) {
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            advance(s, lexer);
        }
        if (lexer->lookahead == '\r' || lexer->lookahead == '\n' || lexer->eof(lexer)) {
            break;
        }
        if (lexer->lookahead == ':') {
            advance(s, lexer);
        }
        bool had_one_minus = false;
        while (lexer->lookahead == '-') {
            had_one_minus = true;
            advance(s, lexer);
        }
        if (!had_one_minus) {
            return false;
        }
        if (lexer->lookahead == ':') {
            advance(s, lexer);
        }
        delimiter_cell_count++;
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            advance(s, lexer);
        }
        if (lexer->lookahead == '|') {
            advance(s, lexer);
            continue;
        }
        if (lexer->lookahead != '\r' && lexer->lookahead != '\n' && !lexer->eof(lexer)) {
            return false;
        }
        break;
    }
    // if the cell counts are not equal then this is not a table
    if (cell_count != delimiter_cell_count) {
        return false;
    }
    return true;
}

static bool parse_pipe_table(Scanner *s, TSLexer *lexer) {
    // PIPE_TABLE_START is zero width
    mark_end(s, lexer);
    bool starting_pipe = false;
    if (lexer->lookahead == '|') {
        starting_pipe = true;
        advance(s, lexer);
    }
    bool simulate = s->simulate;
    bool table = parse_pipe_table_rest(s, lexer, starting_pipe, NULL, NULL);
    s->simulate = simulate;
    if (table) {
        lexer->result_symbol = PIPE_TABLE_START;
        // OKF: table rows are single-line content.
        if (!s->simulate) s->state |= STATE_SINGLE_LINE | STATE_TABLE_ROW;
        return true;
    }
    return false;
}

// OKF: '[' at the start of a block or inside inline content.  One pass decides
// between, in order: a footnote definition (`[^label]:`, block), a task list
// marker (`[ ]` / `[x]`, block), a pipe table header starting with a bracket
// (block), and a footnote reference (`[^label]`, inline).  The block and
// reference starts are zero width; everything read is lookahead.
static bool parse_bracket(Scanner *s, TSLexer *lexer, const bool *valid_symbols,
                          bool consumed_ws) {
    bool want_definition = valid_symbols[FOOTNOTE_DEFINITION_START];
    bool want_task = valid_symbols[TASK_LIST_MARKER_CHECKED] ||
                     valid_symbols[TASK_LIST_MARKER_UNCHECKED];
    bool want_reference = valid_symbols[FOOTNOTE_REFERENCE_START] && !consumed_ws;
    bool want_table = valid_symbols[PIPE_TABLE_START];
    bool want_literal = valid_symbols[LITERAL_OPEN_BRACKET] && !consumed_ws && !s->simulate;
    if (!want_definition && !want_task && !want_reference && !want_table && !want_literal) {
        return false;
    }
    mark_end(s, lexer);
    advance(s, lexer); // '['
    bool task_like = lexer->lookahead == 'x' || lexer->lookahead == 'X' ||
                     lexer->lookahead == ' ' || lexer->lookahead == '\t';
    if (want_literal && lexer->lookahead != '^' && !(want_task && task_like)) {
        // OKF delta I3: a bracket nothing can close is literal text, marked by
        // a zero-width token before it.  At the start of a block the line may
        // still be a table header, checked on the way.
        uint32_t column = is_emphasis_delimiter(lexer->lookahead) ? lexer->get_column(lexer) : 0;
        uint8_t indentation = s->indentation;
        uint8_t col = s->column;
        if (want_table) {
            bool saw_close_bracket = false;
            bool crossed_line = false;
            bool table = parse_pipe_table_rest(s, lexer, false, &saw_close_bracket, &crossed_line);
            s->simulate = false;
            if (table) {
                lexer->result_symbol = PIPE_TABLE_START;
                s->state |= STATE_SINGLE_LINE | STATE_TABLE_ROW;
                return true;
            }
            if (saw_close_bracket || crossed_line) return false;
        }
        if (closer_possible(s, lexer, ']')) return false;
        s->indentation = indentation;
        s->column = col;
        if (column) note_prev(s, CLASS_PUNCTUATION, column);
        lexer->result_symbol = LITERAL_OPEN_BRACKET;
        return true;
    }
    bool reference = false;
    if (lexer->lookahead == '^' && (want_definition || want_reference)) {
        advance(s, lexer);
        size_t label_length = 0;
        while (!lexer->eof(lexer) && !is_newline(lexer->lookahead) &&
               !is_blank(lexer->lookahead) && lexer->lookahead != ']' &&
               lexer->lookahead != '[' && lexer->lookahead != '|' &&
               lexer->lookahead != '\\') {
            label_length++;
            advance(s, lexer);
        }
        if (label_length > 0 && lexer->lookahead == ']') {
            advance(s, lexer);
            if (want_definition && lexer->lookahead == ':' &&
                s->indentation < 4) {
                lexer->result_symbol = FOOTNOTE_DEFINITION_START;
                if (!s->simulate) push_block(s, FOOTNOTE_DEFINITION);
                return true;
            }
            reference = want_reference;
        }
    } else if (want_task &&
               (lexer->lookahead == 'x' || lexer->lookahead == 'X' ||
                lexer->lookahead == ' ' || lexer->lookahead == '\t')) {
        bool checked = lexer->lookahead == 'x' || lexer->lookahead == 'X';
        advance(s, lexer);
        if (lexer->lookahead == ']') {
            advance(s, lexer);
            if (is_blank(lexer->lookahead)) {
                while (is_blank(lexer->lookahead)) advance(s, lexer);
                if (!is_newline(lexer->lookahead) && !lexer->eof(lexer)) {
                    TokenType token =
                        checked ? TASK_LIST_MARKER_CHECKED : TASK_LIST_MARKER_UNCHECKED;
                    if (valid_symbols[token]) {
                        mark_end(s, lexer);
                        s->indentation = 0;
                        lexer->result_symbol = token;
                        return true;
                    }
                }
            }
        }
    }
    if (want_table) {
        // Anything consumed so far contained no pipe, so the table check can
        // carry on from here.  It mutates layout state, which must not leak
        // into a footnote reference emitted instead.
        uint8_t indentation = s->indentation;
        uint8_t column = s->column;
        bool simulate = s->simulate;
        bool table = parse_pipe_table_rest(s, lexer, false, NULL, NULL);
        s->simulate = simulate;
        if (table) {
            lexer->result_symbol = PIPE_TABLE_START;
            if (!s->simulate) s->state |= STATE_SINGLE_LINE | STATE_TABLE_ROW;
            return true;
        }
        s->indentation = indentation;
        s->column = column;
    }
    if (reference) {
        lexer->result_symbol = FOOTNOTE_REFERENCE_START;
        return true;
    }
    return false;
}

// Is the parser at the start of a block, where leading whitespace is
// indentation for the block scanner rather than inline content?
static bool block_start_valid(const bool *valid_symbols) {
    static const TokenType starts[] = {
        INDENTED_CHUNK_START, BLANK_LINE_START, BLOCK_QUOTE_START, ATX_H1_MARKER,
        THEMATIC_BREAK, LIST_MARKER_MINUS, LIST_MARKER_PLUS, LIST_MARKER_STAR,
        LIST_MARKER_PARENTHESIS, LIST_MARKER_DOT, LIST_MARKER_MINUS_DONT_INTERRUPT,
        LIST_MARKER_PLUS_DONT_INTERRUPT, LIST_MARKER_STAR_DONT_INTERRUPT,
        LIST_MARKER_PARENTHESIS_DONT_INTERRUPT, LIST_MARKER_DOT_DONT_INTERRUPT,
        FENCED_CODE_BLOCK_START_BACKTICK, FENCED_CODE_BLOCK_START_TILDE,
        FENCED_CODE_BLOCK_END_BACKTICK, FENCED_CODE_BLOCK_END_TILDE,
        HTML_BLOCK_1_START, HTML_BLOCK_2_START, HTML_BLOCK_3_START,
        HTML_BLOCK_4_START, HTML_BLOCK_5_START, HTML_BLOCK_6_START,
        HTML_BLOCK_7_START, PIPE_TABLE_START, FOOTNOTE_DEFINITION_START,
        SETEXT_H1_UNDERLINE, SETEXT_H2_UNDERLINE, TASK_LIST_MARKER_CHECKED,
        TASK_LIST_MARKER_UNCHECKED,
    };
    for (size_t i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
        if (valid_symbols[starts[i]]) return true;
    }
    return false;
}

typedef enum {
    MID_LINE_NONE,     // nothing applies; nothing was consumed
    MID_LINE_TOKEN,    // a token was emitted
    MID_LINE_DECLINED, // input was consumed and no token applies: stop
    MID_LINE_WHITESPACE, // leading whitespace was consumed into `indentation`
} MidLine;

// OKF: tokens that live in the middle of a line and must be decided before the
// block scanner consumes leading whitespace.
static MidLine parse_mid_line(Scanner *s, TSLexer *lexer, const bool *valid_symbols) {
    int32_t c = lexer->lookahead;
    if (c == '|' && valid_symbols[PIPE_TABLE_EMPTY_CELL]) {
        // `||`: an empty cell, so cell indexes stay column indexes
        mark_end(s, lexer);
        lexer->result_symbol = PIPE_TABLE_EMPTY_CELL;
        return MID_LINE_TOKEN;
    }
    if (c == '|' && valid_symbols[PIPE_TABLE_PIPE]) {
        advance(s, lexer);
        mark_end(s, lexer);
        lexer->result_symbol = PIPE_TABLE_PIPE;
        return MID_LINE_TOKEN;
    }
    if (is_blank(c)) {
        if (valid_symbols[PIPE_TABLE_CELL_LEADING_SPACE]) {
            while (is_blank(lexer->lookahead)) advance(s, lexer);
            mark_end(s, lexer);
            lexer->result_symbol = PIPE_TABLE_CELL_LEADING_SPACE;
            return MID_LINE_TOKEN;
        }
        if (valid_symbols[ATX_HEADING_SPACE]) {
            while (is_blank(lexer->lookahead)) advance(s, lexer);
            mark_end(s, lexer);
            lexer->result_symbol = ATX_HEADING_SPACE;
            return MID_LINE_TOKEN;
        }
        if (valid_symbols[PIPE_TABLE_CELL_TRAILING_SPACE]) {
            size_t length = 0;
            bool tab = false;
            while (is_blank(lexer->lookahead)) {
                tab = tab || lexer->lookahead == '\t';
                length++;
                advance(s, lexer);
            }
            if (lexer->lookahead == '|' || is_newline(lexer->lookahead) ||
                lexer->eof(lexer)) {
                mark_end(s, lexer);
                lexer->result_symbol = PIPE_TABLE_CELL_TRAILING_SPACE;
                return MID_LINE_TOKEN;
            }
            // Inner whitespace: an inline token.  Recorded when an emphasis
            // delimiter run follows (delta I2), else left to the lexer.
            TokenType token = length == 1 && !tab ? WHITESPACE_1 : WHITESPACE_GE_2;
            if (is_emphasis_delimiter(lexer->lookahead) && valid_symbols[token]) {
                mark_end(s, lexer);
                note_prev(s, CLASS_WHITESPACE, lexer->get_column(lexer));
                lexer->result_symbol = token;
                return MID_LINE_TOKEN;
            }
            return MID_LINE_DECLINED;
        }
    }
    // OKF delta I2: whitespace or punctuation right before an emphasis
    // delimiter run is emitted here, so the scanner state records it (see
    // `prev_char_class`).  Anywhere else it is left to the internal lexer.
    if (!block_start_valid(valid_symbols)) {
        if (is_blank(c) && (valid_symbols[WHITESPACE_GE_2] || valid_symbols[WHITESPACE_1])) {
            size_t length = 0;
            bool tab = false;
            while (is_blank(lexer->lookahead)) {
                tab = tab || lexer->lookahead == '\t';
                length++;
                s->indentation += advance(s, lexer);
            }
            TokenType token = length == 1 && !tab ? WHITESPACE_1 : WHITESPACE_GE_2;
            if (is_emphasis_delimiter(lexer->lookahead) && valid_symbols[token]) {
                mark_end(s, lexer);
                note_prev(s, CLASS_WHITESPACE, lexer->get_column(lexer));
                lexer->result_symbol = token;
                return MID_LINE_TOKEN;
            }
            // Not before a delimiter: the whitespace is consumed exactly as
            // the block scanner would have consumed it; carry on from there.
            return MID_LINE_WHITESPACE;
        }
        // Punctuation that no other part of the scanner looks at here.  (`\`
        // starts a backslash escape, `<` may end an html block, and the rest
        // have scanner tokens of their own.)
        int token = punctuation_token(c);
        if (token >= 0 && !is_emphasis_delimiter(c) && c != '`' && c != '[' &&
            c != '\\' && c != '<' && valid_symbols[token] &&
            !(c == ']' && valid_symbols[FOOTNOTE_DEFINITION_MARKER_END])) {
            advance(s, lexer);
            if (!is_emphasis_delimiter(lexer->lookahead)) return MID_LINE_DECLINED;
            mark_end(s, lexer);
            note_prev(s, CLASS_PUNCTUATION, lexer->get_column(lexer));
            lexer->result_symbol = (TSSymbol)token;
            return MID_LINE_TOKEN;
        }
    }
    if (valid_symbols[FOOTNOTE_LABEL]) {
        size_t length = 0;
        while (!lexer->eof(lexer) && !is_newline(lexer->lookahead) &&
               !is_blank(lexer->lookahead) && lexer->lookahead != ']' &&
               lexer->lookahead != '[' && lexer->lookahead != '|' &&
               lexer->lookahead != '\\') {
            length++;
            advance(s, lexer);
        }
        if (length > 0) {
            mark_end(s, lexer);
            lexer->result_symbol = FOOTNOTE_LABEL;
            return MID_LINE_TOKEN;
        }
        return MID_LINE_NONE;
    }
    if (c == ']' && valid_symbols[FOOTNOTE_REFERENCE_END]) {
        advance(s, lexer);
        mark_end(s, lexer);
        if (is_emphasis_delimiter(lexer->lookahead)) {
            note_prev(s, CLASS_PUNCTUATION, lexer->get_column(lexer));
        }
        lexer->result_symbol = FOOTNOTE_REFERENCE_END;
        return MID_LINE_TOKEN;
    }
    if (c == ']' && valid_symbols[FOOTNOTE_DEFINITION_MARKER_END]) {
        advance(s, lexer);
        if (lexer->lookahead != ':') return MID_LINE_DECLINED;
        advance(s, lexer);
        while (is_blank(lexer->lookahead)) advance(s, lexer);
        mark_end(s, lexer);
        s->indentation = 0;
        lexer->result_symbol = FOOTNOTE_DEFINITION_MARKER_END;
        return MID_LINE_TOKEN;
    }
    return MID_LINE_NONE;
}

typedef enum {
    PUNCTUATION_NOT_APPLICABLE, // nothing consumed
    PUNCTUATION_EMITTED,
    PUNCTUATION_DECLINED, // input consumed, no token
} BlockStartPunctuation;

// OKF delta I2: punctuation starting a block and followed by an emphasis
// delimiter run is emitted and recorded (see `prev_char_class`).  The pipe
// table check still runs: should the line really be a pipe-less table header,
// the table start token also covers that one character (the tree keeps its
// shape; the first cell starts one character late).
static BlockStartPunctuation block_start_punctuation(Scanner *s, TSLexer *lexer,
                                                     const bool *valid_symbols) {
    int32_t c = lexer->lookahead;
    int token = punctuation_token(c);
    if (token < 0 || c == '|' || c == '\\' || s->simulate || !valid_symbols[token]) {
        return PUNCTUATION_NOT_APPLICABLE;
    }
    bool want_table = valid_symbols[PIPE_TABLE_START];
    lexer->mark_end(lexer);
    advance(s, lexer);
    if (is_emphasis_delimiter(lexer->lookahead)) {
        lexer->mark_end(lexer);
        uint32_t column = lexer->get_column(lexer);
        if (want_table) {
            bool table = parse_pipe_table_rest(s, lexer, false, NULL, NULL);
            s->simulate = false;
            if (table) {
                lexer->result_symbol = PIPE_TABLE_START;
                s->state |= STATE_SINGLE_LINE | STATE_TABLE_ROW;
                return PUNCTUATION_EMITTED;
            }
        }
        s->indentation = 0;
        s->column = 0;
        note_prev(s, CLASS_PUNCTUATION, column);
        lexer->result_symbol = (TSSymbol)token;
        return PUNCTUATION_EMITTED;
    }
    if (want_table) {
        bool table = parse_pipe_table_rest(s, lexer, false, NULL, NULL);
        s->simulate = false;
        if (table) {
            lexer->result_symbol = PIPE_TABLE_START;
            s->state |= STATE_SINGLE_LINE | STATE_TABLE_ROW;
            return PUNCTUATION_EMITTED;
        }
    }
    return PUNCTUATION_DECLINED;
}

static bool md_scan(Scanner *s, TSLexer *lexer, const bool *valid_symbols) {
    // A normal tree-sitter rule decided that the current branch is invalid and
    // now "requests" an error to stop the branch
    if (valid_symbols[TRIGGER_ERROR]) {
        return error(lexer);
    }

    // Close the inner most block after the next line break as requested.  See
    // `$._close_block` in grammar/block.js
    if (valid_symbols[CLOSE_BLOCK]) {
        s->state |= STATE_CLOSE_BLOCK;
        lexer->result_symbol = CLOSE_BLOCK;
        return true;
    }

    // if we are at the end of the file and there are still open blocks close
    // them all
    if (lexer->eof(lexer)) {
        if (valid_symbols[TOKEN_EOF]) {
            lexer->result_symbol = TOKEN_EOF;
            return true;
        }
        // OKF: a block that needs content ends the input (`- ` with no final
        // newline).  One zero-width blank line gives it that content; never
        // more than one, so this cannot loop.
        if (valid_symbols[BLANK_LINE_START] && !(s->state & STATE_EOF_BLANK_LINE) &&
            !s->simulate) {
            s->state |= STATE_EOF_BLANK_LINE;
            lexer->result_symbol = BLANK_LINE_START;
            return true;
        }
        if (s->open_blocks.size > 0) {
            lexer->result_symbol = BLOCK_CLOSE;
            if (!s->simulate) pop_block(s);
            return true;
        }
        return false;
    }

    if (!(s->state & STATE_MATCHING)) {
        bool consumed_ws = false;
        if (!s->simulate) {
            MidLine mid = parse_mid_line(s, lexer, valid_symbols);
            if (mid == MID_LINE_TOKEN) return true;
            if (mid == MID_LINE_DECLINED) return false;
            consumed_ws = mid == MID_LINE_WHITESPACE;
        }

        // Parse any preceding whitespace and remember its length.  This makes
        // a lot of parsing quite a bit easier.
        size_t ws_chars = 0;
        bool ws_tab = false;
        for (;;) {
            if (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                ws_tab = ws_tab || lexer->lookahead == '\t';
                ws_chars++;
                s->indentation += advance(s, lexer);
                consumed_ws = true;
            } else {
                break;
            }
        }
        // OKF delta I2: where the whitespace ends is where an inline
        // whitespace token would end, should a delimiter run follow.  Every
        // block token marks its own end further on.
        s->leading_ws_column = 0;
        if (consumed_ws && ws_chars > 0 && !s->simulate &&
            is_emphasis_delimiter(lexer->lookahead) && block_start_valid(valid_symbols)) {
            lexer->mark_end(lexer);
            s->leading_ws_column = (uint16_t)lexer->get_column(lexer);
            s->leading_ws_single = ws_chars == 1 && !ws_tab;
        }
        // OKF: trailing whitespace in inline content is an inline token (a
        // hard line break needs it), not part of the line ending.
        if (consumed_ws && is_newline(lexer->lookahead) &&
            valid_symbols[CODE_SPAN_START] && !valid_symbols[BLANK_LINE_START]) {
            return false;
        }
        // We are not matching.  This is where the parsing logic for most
        // "normal" tokens is.  Most importantly parsing logic for the start of
        // new blocks.
        if (valid_symbols[INDENTED_CHUNK_START] &&
            !valid_symbols[NO_INDENTED_CHUNK]) {
            if (s->indentation >= 4 && lexer->lookahead != '\n' &&
                lexer->lookahead != '\r') {
                lexer->result_symbol = INDENTED_CHUNK_START;
                if (!s->simulate) push_block(s, INDENTED_CODE_BLOCK);
                s->indentation -= 4;
                return true;
            }
        }
        // Decide which tokens to consider based on the first non-whitespace
        // character
        switch (lexer->lookahead) {
            case '\r':
            case '\n':
                if (valid_symbols[BLANK_LINE_START]) {
                    // A blank line token is actually just 0 width, so do not
                    // consume the characters
                    lexer->result_symbol = BLANK_LINE_START;
                    return true;
                }
                break;
            case '`':
                return parse_backtick(s, lexer, valid_symbols, consumed_ws);
            case '~':
                return parse_tilde(s, lexer, valid_symbols, consumed_ws);
            case '*':
                return parse_star(s, lexer, valid_symbols, consumed_ws);
            case '_':
                return parse_underscore(s, lexer, valid_symbols, consumed_ws);
            case '>':
                // A '>' could mark the beginning of a block quote
                return parse_block_quote(s, lexer, valid_symbols);
            case '#':
                // A '#' could mark a atx heading
                return parse_atx_heading(s, lexer, valid_symbols);
            case '=':
                // A '=' could mark a setext underline
                if (!valid_symbols[SETEXT_H1_UNDERLINE] && !consumed_ws) {
                    BlockStartPunctuation result = block_start_punctuation(s, lexer, valid_symbols);
                    if (result != PUNCTUATION_NOT_APPLICABLE) return result == PUNCTUATION_EMITTED;
                }
                return parse_setext_underline(s, lexer, valid_symbols);
            case '+':
                // A '+' could be a list marker
                return parse_plus(s, lexer, valid_symbols);
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
                // A number could be a list marker (if followed by a dot or a
                // parenthesis)
                return parse_ordered_list_marker(s, lexer, valid_symbols);
            case '-':
                // A minus could mark a list marker, a thematic break, a setext
                // underline or (OKF) the frontmatter opener
                return parse_minus(s, lexer, valid_symbols);
            case '<':
                // A < could mark the beginning of a html block
                return parse_html_block(s, lexer, valid_symbols);
            case '[':
                return parse_bracket(s, lexer, valid_symbols, consumed_ws);
        }
        if (lexer->lookahead != '\r' && lexer->lookahead != '\n' &&
            valid_symbols[PIPE_TABLE_START]) {
            if (!consumed_ws) {
                BlockStartPunctuation result = block_start_punctuation(s, lexer, valid_symbols);
                if (result != PUNCTUATION_NOT_APPLICABLE) return result == PUNCTUATION_EMITTED;
            }
            return parse_pipe_table(s, lexer);
        }
    } else { // we are in the state of trying to match all currently open blocks
        bool partial_success = false;
        while (s->matched < (uint8_t)s->open_blocks.size) {
            if (s->matched == (uint8_t)s->open_blocks.size - 1 &&
                (s->state & STATE_CLOSE_BLOCK)) {
                if (!partial_success) s->state &= ~STATE_CLOSE_BLOCK;
                break;
            }
            if (match(s, lexer, s->open_blocks.items[s->matched])) {
                partial_success = true;
                s->matched++;
            } else {
                if (s->state & STATE_WAS_SOFT_LINE_BREAK) {
                    s->state &= (~STATE_MATCHING);
                }
                break;
            }
        }
        if (partial_success) {
            if (s->matched == s->open_blocks.size) {
                s->state &= (~STATE_MATCHING);
            }
            if (is_emphasis_delimiter(lexer->lookahead)) {
                // content after a container marker starts a line
                note_prev(s, CLASS_WHITESPACE, lexer->get_column(lexer));
            }
            lexer->result_symbol = BLOCK_CONTINUATION;
            return true;
        }

        if (!(s->state & STATE_WAS_SOFT_LINE_BREAK)) {
            lexer->result_symbol = BLOCK_CLOSE;
            pop_block(s);
            if (s->matched == s->open_blocks.size) {
                s->state &= (~STATE_MATCHING);
            }
            return true;
        }
    }

    // The parser just encountered a line break.  Setup the state
    // correspondingly
    if ((valid_symbols[LINE_ENDING] || valid_symbols[SOFT_LINE_ENDING] ||
         valid_symbols[PIPE_TABLE_LINE_ENDING]) &&
        (lexer->lookahead == '\n' || lexer->lookahead == '\r')) {
        if (lexer->lookahead == '\r') {
            advance(s, lexer);
            if (lexer->lookahead == '\n') {
                advance(s, lexer);
            }
        } else {
            advance(s, lexer);
        }
        s->indentation = 0;
        s->column = 0;
        // OKF: a single-line construct's ending is only ever a table row
        // ending or a plain line ending.
        bool single_line = (s->state & STATE_SINGLE_LINE) &&
                           !valid_symbols[PIPE_TABLE_LINE_ENDING];
        if (!(s->state & STATE_CLOSE_BLOCK) && !single_line &&
            (valid_symbols[SOFT_LINE_ENDING] ||
             valid_symbols[PIPE_TABLE_LINE_ENDING])) {
            lexer->mark_end(lexer);
            for (;;) {
                if (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                    s->indentation += advance(s, lexer);
                } else {
                    break;
                }
            }
            s->simulate = true;
            uint8_t matched_temp = s->matched;
            s->matched = 0;
            bool one_will_be_matched = false;
            while (s->matched < (uint8_t)s->open_blocks.size) {
                if (match(s, lexer, s->open_blocks.items[s->matched])) {
                    s->matched++;
                    one_will_be_matched = true;
                } else {
                    break;
                }
            }
            bool all_will_be_matched = s->matched == s->open_blocks.size;
            uint8_t state_temp = s->state;
            uint8_t fenced_temp = s->fenced_code_block_delimiter_length;
            bool interrupts =
                !lexer->eof(lexer) && md_scan(s, lexer, paragraph_interrupt_symbols);
            s->state = state_temp;
            s->fenced_code_block_delimiter_length = fenced_temp;
            s->simulate = false;
            if (!interrupts) {
                s->matched = matched_temp;
                // If the last line break ended a paragraph and no new block
                // opened, the last line break should have been a soft line
                // break.  Reset the counter for matched blocks
                s->matched = 0;
                s->indentation = 0;
                s->column = 0;
                // If there is at least one open block, we should be in the
                // matching state.  Also set the matching flag if a
                // `$._soft_line_break_marker` can be emitted so it does get
                // emitted.
                if (one_will_be_matched) {
                    s->state |= STATE_MATCHING;
                } else {
                    s->state &= (~STATE_MATCHING);
                }
                if (valid_symbols[PIPE_TABLE_LINE_ENDING]) {
                    if (all_will_be_matched) {
                        lexer->result_symbol = PIPE_TABLE_LINE_ENDING;
                        s->state |= STATE_SINGLE_LINE | STATE_TABLE_ROW;
                        return true;
                    }
                } else {
                    lexer->result_symbol = SOFT_LINE_ENDING;
                    // reset some state variables
                    s->state |= STATE_WAS_SOFT_LINE_BREAK;
                    return true;
                }
            } else {
                s->matched = matched_temp;
            }
            s->indentation = 0;
            s->column = 0;
        }
        if (valid_symbols[LINE_ENDING]) {
            // If the last line break ended a paragraph and no new block opened,
            // the last line break should have been a soft line break.  Reset
            // the counter for matched blocks
            s->matched = 0;
            // If there is at least one open block, we should be in the matching
            // state.  Also set the matching flag if a
            // `$._soft_line_break_marker` can be emitted so it does get
            // emitted.
            if (s->open_blocks.size > 0) {
                s->state |= STATE_MATCHING;
            } else {
                s->state &= (~STATE_MATCHING);
            }
            // reset some state variables
            s->state &= (~STATE_WAS_SOFT_LINE_BREAK);
            s->state &= (~(STATE_SINGLE_LINE | STATE_TABLE_ROW));
            // OKF: a hard line ending ends any inline content, so inline
            // state is reset (canonical states merge, see `serialize`).
            s->inline_state = 0;
            s->code_span_delimiter_length = 0;
            s->num_emphasis_delimiters_left = 0;
            lexer->result_symbol = LINE_ENDING;
            return true;
        }
    }
    return false;
}

/* ========================================================================
 * OKF-YAML frontmatter scanner (new)
 *
 * See grammar/frontmatter.js for the token contract.  In short: at a line
 * break the newline, blank lines and the next line's indentation are skipped
 * (they become padding) and one zero-width structural token is emitted at the
 * start of the next line's content.  Everything else is an ordinary token.
 * Whenever the natural token is not valid in the current parse state the
 * scanner falls back to `_fm_unsupported`, so frontmatter never ERRORs.
 * ======================================================================== */

static void fm_reset(Scanner *s) {
    s->fm_active = false;
    s->fm_root_known = false;
    s->fm_unsupported_rest = false;
    s->fm_force_unsupported = false;
    s->fm_line = FM_MID_LINE;
    s->fm_depth = 1;
    s->fm_line_indent = 0;
    s->fm_line_chars = 0;
    s->fm_node_column = -1;
    s->fm_node_line_start = false;
    s->fm_levels[0].indent = 0;
    s->fm_levels[0].kind = FM_LEVEL_BLOCK;
}

static inline void fm_advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static inline bool fm_at_eol(TSLexer *lexer) {
    return is_newline(lexer->lookahead) || lexer->eof(lexer);
}

static inline bool fm_is_flow_indicator(int32_t c) {
    return c == ',' || c == '[' || c == ']' || c == '{' || c == '}';
}

static inline FmLevel *fm_top(Scanner *s) { return &s->fm_levels[s->fm_depth - 1]; }

// Open an indentation level.  Fails at FM_MAX_DEPTH: every caller then
// declines to open the block (the line becomes opaque), because a level the
// parser opens but the scanner does not track could never be closed, and
// the closing `---` would not be recognised (P4).
static bool fm_push(Scanner *s, int16_t indent, FmLevelKind kind) {
    if (s->fm_depth >= FM_MAX_DEPTH) return false;
    s->fm_levels[s->fm_depth].indent = indent;
    s->fm_levels[s->fm_depth].kind = (uint8_t)kind;
    s->fm_depth++;
    return true;
}

static bool fm_emit(Scanner *s, TSLexer *lexer, TokenType token, FmLine line) {
    s->fm_line = (uint8_t)line;
    if (token != FM_ANCHOR && token != FM_TAG) {
        // the node the properties belonged to has been emitted (or the line
        // ended): its position no longer applies
        s->fm_node_column = -1;
        s->fm_node_line_start = false;
    }
    lexer->result_symbol = token;
    return true;
}

// The column of the cursor with tabs in the line's indentation counted as
// in `fm_peek_next_line` (see `Scanner.fm_line_indent`).
static int16_t fm_column(Scanner *s, TSLexer *lexer) {
    return (int16_t)(s->fm_line_indent + (int32_t)lexer->get_column(lexer) - s->fm_line_chars);
}

// Skip a line's indentation at the cursor and record it as the current
// line's (the cursor is at the start of the line).
static void fm_skip_indentation(Scanner *s, TSLexer *lexer) {
    int16_t indent = 0;
    uint16_t chars = 0;
    while (is_blank(lexer->lookahead)) {
        indent = lexer->lookahead == '\t'
                     ? (int16_t)((indent / FM_TAB_WIDTH + 1) * FM_TAB_WIDTH)
                     : (int16_t)(indent + 1);
        chars++;
        fm_advance(lexer);
    }
    s->fm_line_indent = indent;
    s->fm_line_chars = chars;
}

// The rest of the line as one opaque token.
static bool fm_unsupported_to_eol(Scanner *s, TSLexer *lexer) {
    while (!fm_at_eol(lexer)) fm_advance(lexer);
    lexer->mark_end(lexer);
    return fm_emit(s, lexer, FM_UNSUPPORTED, FM_MID_LINE);
}

// The indentation threshold a folded continuation line must exceed: the
// enclosing block's indentation.  A node that starts a line sits at an
// `_fm_indent` level of its own, so its enclosing block is the one below.
static int16_t fm_threshold(Scanner *s, bool line_start) {
    if (line_start) {
        return s->fm_depth >= 2 ? s->fm_levels[s->fm_depth - 2].indent : -1;
    }
    return fm_top(s)->indent;
}

// At a line start, after its indentation: is this line `---` or `...`
// followed by whitespace or its end?  Consumes what it reads.
static bool fm_document_marker(TSLexer *lexer, int32_t marker) {
    for (int i = 0; i < 3; i++) {
        if (lexer->lookahead != marker) return false;
        fm_advance(lexer);
    }
    return is_blank(lexer->lookahead) || fm_at_eol(lexer);
}

// Consume (as part of the current token) a line ending, blank lines and the
// next line's indentation.  Returns that indentation, or -1 at end of input.
static int16_t fm_consume_to_content(TSLexer *lexer) {
    int16_t indent = 0;
    for (;;) {
        if (lexer->eof(lexer)) return -1;
        if (lexer->lookahead == '\r' || lexer->lookahead == '\n') {
            fm_advance(lexer);
            indent = 0;
        } else if (lexer->lookahead == ' ') {
            fm_advance(lexer);
            indent++;
        } else if (lexer->lookahead == '\t') {
            fm_advance(lexer);
            indent = (int16_t)((indent / FM_TAB_WIDTH + 1) * FM_TAB_WIDTH);
        } else {
            return indent;
        }
    }
}

// A plain scalar in block context.  Decides between a key (followed by `:` and
// whitespace), a value (with folded continuation lines) and, when neither fits
// the parse state, an opaque line.  The first character has not been consumed
// unless `consumed` says so.
static bool fm_plain(Scanner *s, TSLexer *lexer, const bool *valid_symbols,
                     bool line_start, uint32_t column, bool consumed) {
    bool want_key = valid_symbols[FM_KEY];
    bool want_plain = valid_symbols[FM_PLAIN];
    bool colon = false;
    bool comment = false;
    bool pending_ws = false;
    if (consumed) lexer->mark_end(lexer);
    while (!fm_at_eol(lexer)) {
        int32_t c = lexer->lookahead;
        if (c == ':') {
            fm_advance(lexer);
            if (is_blank(lexer->lookahead) || fm_at_eol(lexer)) {
                colon = true;
                break;
            }
            lexer->mark_end(lexer);
            pending_ws = false;
            continue;
        }
        if (is_blank(c)) {
            fm_advance(lexer);
            pending_ws = true;
            continue;
        }
        if (c == '#' && pending_ws) {
            comment = true;
            break;
        }
        fm_advance(lexer);
        lexer->mark_end(lexer);
        pending_ws = false;
    }

    if (colon) {
        if (want_key && (line_start || fm_push(s, (int16_t)column, FM_LEVEL_BLOCK))) {
            return fm_emit(s, lexer, FM_KEY, FM_MID_LINE);
        }
        // `key: a: b` is not YAML; keep the whole value opaque.
        if (valid_symbols[FM_UNSUPPORTED]) return fm_unsupported_to_eol(s, lexer);
        return false;
    }
    if (!want_plain) {
        // A mapping entry position with no key on the line.
        if (valid_symbols[FM_UNSUPPORTED]) return fm_unsupported_to_eol(s, lexer);
        return false;
    }
    if (!comment) {
        // Folded continuation lines: deeper than the enclosing block, not a
        // comment line, not a document marker.
        int16_t threshold = fm_threshold(s, line_start);
        while (is_newline(lexer->lookahead)) {
            int16_t indent = fm_consume_to_content(lexer);
            if (indent < 0 || indent <= threshold) break;
            if (lexer->lookahead == '#') break;
            if (indent == 0 && (lexer->lookahead == '-' || lexer->lookahead == '.')) {
                // what the marker check reads is the line's first content
                int32_t marker = lexer->lookahead;
                if (fm_document_marker(lexer, marker)) break;
            }
            // The line folds in unless it reads as `key: value`, which is
            // not YAML either way but is far more useful kept as an entry.
            // So nothing on it is marked until that is known; a key-like
            // line leaves the token ending on the previous line.  (Trailing
            // whitespace before a comment on a folded line ends up in the
            // token: the price of not being able to mark backwards.)
            bool stop = false;
            bool key_like = false;
            pending_ws = false;
            while (!fm_at_eol(lexer)) {
                int32_t c = lexer->lookahead;
                if (is_blank(c)) {
                    fm_advance(lexer);
                    pending_ws = true;
                    continue;
                }
                if (c == '#' && pending_ws) {
                    stop = true;
                    break;
                }
                fm_advance(lexer);
                if (c == ':' && (is_blank(lexer->lookahead) || fm_at_eol(lexer))) {
                    key_like = true;
                    break;
                }
                pending_ws = false;
            }
            if (key_like) break;
            lexer->mark_end(lexer);
            if (stop) break;
        }
    }
    return fm_emit(s, lexer, FM_PLAIN, FM_MID_LINE);
}

// A plain scalar inside a flow collection: single line, ended by a flow
// indicator, a `: ` or a comment.
static bool fm_flow_plain(Scanner *s, TSLexer *lexer) {
    bool any = false;
    bool pending_ws = false;
    while (!fm_at_eol(lexer)) {
        int32_t c = lexer->lookahead;
        if (fm_is_flow_indicator(c)) break;
        if (c == ':') {
            fm_advance(lexer);
            int32_t n = lexer->lookahead;
            if (is_blank(n) || fm_at_eol(lexer) || fm_is_flow_indicator(n)) break;
            lexer->mark_end(lexer);
            any = true;
            pending_ws = false;
            continue;
        }
        if (is_blank(c)) {
            fm_advance(lexer);
            pending_ws = true;
            continue;
        }
        if (c == '#' && pending_ws) break;
        fm_advance(lexer);
        lexer->mark_end(lexer);
        any = true;
        pending_ws = false;
    }
    if (!any) return false;
    return fm_emit(s, lexer, FM_FLOW_PLAIN, FM_MID_LINE);
}

// A quoted scalar.  Multi-line is allowed; the token is byte faithful and
// hosts unquote (spec D3).  An unterminated scalar, or a double-quoted one
// with an escaped line break (out of subset, spec §5.1), is opaque.
static bool fm_quoted(Scanner *s, TSLexer *lexer, const bool *valid_symbols,
                      bool line_start, uint32_t column, bool in_flow) {
    int32_t quote = lexer->lookahead;
    TokenType token = quote == '"' ? FM_DOUBLE_QUOTE : FM_SINGLE_QUOTE;
    int16_t threshold = fm_threshold(s, line_start);
    bool escaped_line_break = false;
    bool closed = false;
    fm_advance(lexer);
    lexer->mark_end(lexer);
    for (;;) {
        if (lexer->eof(lexer)) break;
        int32_t c = lexer->lookahead;
        if (is_newline(c)) {
            int16_t indent = fm_consume_to_content(lexer);
            if (indent < 0) break;
            if (!in_flow && indent <= threshold) break;
            if (indent == 0 && (lexer->lookahead == '-' || lexer->lookahead == '.')) {
                int32_t marker = lexer->lookahead;
                if (fm_document_marker(lexer, marker)) break;
            }
            lexer->mark_end(lexer);
            continue;
        }
        if (quote == '"' && c == '\\') {
            fm_advance(lexer);
            lexer->mark_end(lexer); // the backslash is content, whatever follows
            if (is_newline(lexer->lookahead)) {
                escaped_line_break = true;
                continue;
            }
            if (!lexer->eof(lexer)) fm_advance(lexer);
            lexer->mark_end(lexer);
            continue;
        }
        if (c == quote) {
            fm_advance(lexer);
            if (quote == '\'' && lexer->lookahead == '\'') {
                fm_advance(lexer);
                lexer->mark_end(lexer);
                continue;
            }
            lexer->mark_end(lexer);
            closed = true;
            break;
        }
        fm_advance(lexer);
        lexer->mark_end(lexer);
    }
    if (!closed) {
        // The token ends with the last content line; the line that stopped
        // the scan is left alone.
        if (valid_symbols[FM_UNSUPPORTED]) return fm_emit(s, lexer, FM_UNSUPPORTED, FM_MID_LINE);
        return false;
    }
    if (escaped_line_break && valid_symbols[FM_UNSUPPORTED]) {
        // out of subset: opaque, through the end of the closing quote's line
        return fm_unsupported_to_eol(s, lexer);
    }
    if (in_flow) return fm_emit(s, lexer, token, FM_MID_LINE);

    // Block context: what follows decides key / value / opaque.
    while (is_blank(lexer->lookahead)) fm_advance(lexer);
    bool key = false;
    if (lexer->lookahead == ':') {
        fm_advance(lexer);
        key = is_blank(lexer->lookahead) || fm_at_eol(lexer);
        if (!key) goto garbage;
    } else if (!fm_at_eol(lexer) && lexer->lookahead != '#') {
        goto garbage;
    }
    if (key) {
        if (valid_symbols[FM_KEY] &&
            (line_start || fm_push(s, (int16_t)column, FM_LEVEL_BLOCK))) {
            return fm_emit(s, lexer, token, FM_MID_LINE);
        }
        goto garbage;
    }
    if (!valid_symbols[FM_PLAIN] && valid_symbols[FM_UNSUPPORTED]) {
        // a mapping entry position: a quoted scalar with no `:` is no entry
        goto garbage;
    }
    return fm_emit(s, lexer, token, FM_MID_LINE);

garbage:
    if (valid_symbols[FM_UNSUPPORTED]) return fm_unsupported_to_eol(s, lexer);
    return fm_emit(s, lexer, token, FM_MID_LINE);
}

// `|` / `>`, the header's indicators and optional comment, then the body: every
// line indented at least as deep as the first non-blank one, which must be
// deeper than the enclosing block.
static bool fm_block_scalar(Scanner *s, TSLexer *lexer, const bool *valid_symbols,
                            bool line_start) {
    fm_advance(lexer);
    int explicit_indent = 0;
    for (int i = 0; i < 2; i++) {
        if (lexer->lookahead == '-' || lexer->lookahead == '+') {
            fm_advance(lexer);
        } else if (lexer->lookahead >= '1' && lexer->lookahead <= '9' &&
                   explicit_indent == 0) {
            explicit_indent = lexer->lookahead - '0';
            fm_advance(lexer);
        }
    }
    bool pending_ws = false;
    while (is_blank(lexer->lookahead)) {
        fm_advance(lexer);
        pending_ws = true;
    }
    if (lexer->lookahead == '#' && pending_ws) {
        while (!fm_at_eol(lexer)) fm_advance(lexer);
    }
    if (!fm_at_eol(lexer)) {
        if (valid_symbols[FM_UNSUPPORTED]) return fm_unsupported_to_eol(s, lexer);
        return false;
    }
    lexer->mark_end(lexer);

    int16_t threshold = fm_threshold(s, line_start);
    int16_t content_indent =
        explicit_indent > 0 ? (int16_t)((threshold < 0 ? 0 : threshold) + explicit_indent) : -1;
    while (is_newline(lexer->lookahead)) {
        fm_advance(lexer);
        int16_t indent = 0;
        while (is_blank(lexer->lookahead)) {
            indent = lexer->lookahead == '\t'
                         ? (int16_t)((indent / FM_TAB_WIDTH + 1) * FM_TAB_WIDTH)
                         : (int16_t)(indent + 1);
            fm_advance(lexer);
        }
        if (lexer->eof(lexer)) break;
        if (is_newline(lexer->lookahead)) continue; // blank line
        if (content_indent < 0) {
            if (indent <= threshold) break;
            content_indent = indent;
        }
        if (indent < content_indent) break;
        if (indent == 0 && (lexer->lookahead == '-' || lexer->lookahead == '.')) {
            int32_t marker = lexer->lookahead;
            if (fm_document_marker(lexer, marker)) break;
        }
        while (!fm_at_eol(lexer)) fm_advance(lexer);
        lexer->mark_end(lexer);
    }
    return fm_emit(s, lexer, FM_BLOCK_SCALAR, FM_MID_LINE);
}

// After a property at a mapping entry position: does the rest of the line
// read as further properties, then a key the scanner will lex as one (a plain
// scalar not starting with an indicator, or a closed quoted scalar), then `:`
// and whitespace?  Everything read is lookahead.
static bool fm_key_follows(TSLexer *lexer) {
    for (;;) {
        while (is_blank(lexer->lookahead)) fm_advance(lexer);
        if (lexer->lookahead != '&' && lexer->lookahead != '!') break;
        while (!fm_at_eol(lexer) && !is_blank(lexer->lookahead)) fm_advance(lexer);
    }
    int32_t c = lexer->lookahead;
    if (c == '"' || c == '\'') {
        fm_advance(lexer);
        for (;;) {
            if (fm_at_eol(lexer)) return false;
            int32_t q = lexer->lookahead;
            fm_advance(lexer);
            if (c == '"' && q == '\\') {
                if (fm_at_eol(lexer)) return false;
                fm_advance(lexer);
                continue;
            }
            if (q == c) {
                if (c == '\'' && lexer->lookahead == '\'') {
                    fm_advance(lexer);
                    continue;
                }
                break;
            }
        }
        while (is_blank(lexer->lookahead)) fm_advance(lexer);
        if (lexer->lookahead != ':') return false;
        fm_advance(lexer);
        return is_blank(lexer->lookahead) || fm_at_eol(lexer);
    }
    // plain: the same start rules as `fm_token`
    if (fm_at_eol(lexer) || fm_is_flow_indicator(c) || c == '#' || c == '&' || c == '*' ||
        c == '!' || c == '|' || c == '>' || c == '%' || c == '@' || c == '`') {
        return false;
    }
    if (c == '-' || c == '?' || c == ':') {
        fm_advance(lexer);
        if (is_blank(lexer->lookahead) || fm_at_eol(lexer)) return false;
    }
    bool pending_ws = false;
    while (!fm_at_eol(lexer)) {
        int32_t d = lexer->lookahead;
        if (d == '#' && pending_ws) return false;
        fm_advance(lexer);
        if (d == ':' && (is_blank(lexer->lookahead) || fm_at_eol(lexer))) return true;
        pending_ws = is_blank(d);
    }
    return false;
}

// `&anchor`, `*alias`, `!tag` / `!!tag` / `!<verbatim>`.
static bool fm_emit_property(Scanner *s, TSLexer *lexer, TokenType token,
                             int16_t column, bool line_start) {
    if (token != FM_ALIAS && s->fm_node_column < 0) {
        s->fm_node_column = column;
        s->fm_node_line_start = line_start;
    }
    return fm_emit(s, lexer, token, FM_MID_LINE);
}

static bool fm_property(Scanner *s, TSLexer *lexer, const bool *valid_symbols,
                        TokenType token, bool in_flow, int16_t column, bool line_start) {
    fm_advance(lexer);
    size_t length = 0;
    if (token == FM_TAG && lexer->lookahead == '<') {
        while (!fm_at_eol(lexer) && lexer->lookahead != '>') {
            fm_advance(lexer);
            length++;
        }
        if (lexer->lookahead == '>') fm_advance(lexer);
    }
    while (!fm_at_eol(lexer) && !is_blank(lexer->lookahead) &&
           !(in_flow && fm_is_flow_indicator(lexer->lookahead))) {
        fm_advance(lexer);
        length++;
    }
    bool ok = token == FM_TAG || length > 0; // a lone `!` is the non-specific tag
    if (ok && valid_symbols[token] && !in_flow && valid_symbols[FM_KEY] &&
        !valid_symbols[FM_PLAIN] && token != FM_ALIAS) {
        // Where only a mapping entry can start, a property must be followed by
        // a key on its line (`&a key: v`); a line that is only properties is
        // not an entry, so it is opaque.
        lexer->mark_end(lexer);
        if (fm_key_follows(lexer)) return fm_emit_property(s, lexer, token, column, line_start);
        if (valid_symbols[FM_UNSUPPORTED]) return fm_unsupported_to_eol(s, lexer);
        return false;
    }
    if (ok && valid_symbols[token]) {
        lexer->mark_end(lexer);
        if (token == FM_ALIAS && !in_flow) {
            // An alias is a whole node: in block context only a comment may
            // follow it on its line (`*a: b`, an alias key, is out of subset).
            bool after_space = false;
            while (is_blank(lexer->lookahead)) {
                fm_advance(lexer);
                after_space = true;
            }
            if (!fm_at_eol(lexer) && !(lexer->lookahead == '#' && after_space)) {
                if (valid_symbols[FM_UNSUPPORTED]) return fm_unsupported_to_eol(s, lexer);
            }
        }
        return fm_emit_property(s, lexer, token, column, line_start);
    }
    if (valid_symbols[FM_UNSUPPORTED]) return fm_unsupported_to_eol(s, lexer);
    return false;
}

// A recursive-descent check that a flow collection is exactly what the flow
// grammar accepts (grammar/frontmatter.js: `flow_mapping`, `flow_sequence`),
// tokenized exactly as `fm_token` would tokenize it, run before the scanner
// commits to the opening bracket.  A scanner token cannot be taken back once
// the parser has it, so a collection is only opened when it is known to
// close; anything else is opaque (spec §5.1, P4).
typedef struct {
    TSLexer *lexer;
    int16_t threshold; // continuation lines must be deeper than this
    int depth;
} FlowCheck;

// Skip whitespace, line breaks and comments inside a flow collection.
// Returns false at the end of the collection's region: end of input, a
// document marker, or a line not indented past the enclosing block.
static bool flow_skip(FlowCheck *f, bool after_space) {
    TSLexer *lexer = f->lexer;
    for (;;) {
        int32_t c = lexer->lookahead;
        if (lexer->eof(lexer)) return false;
        if (is_blank(c)) {
            fm_advance(lexer);
            after_space = true;
        } else if (c == '#' && after_space) {
            while (!fm_at_eol(lexer)) fm_advance(lexer);
        } else if (is_newline(c)) {
            int16_t indent = fm_consume_to_content(lexer);
            if (indent < 0) return false;
            int32_t first = lexer->lookahead;
            // A column-0 line starting `-` or `.` may be a document marker;
            // checking would consume it, so the region conservatively ends.
            if (indent == 0 && (first == '-' || first == '.')) return false;
            if (indent <= f->threshold && first != '}' && first != ']') return false;
            after_space = true;
        } else {
            return true;
        }
    }
}

// Anchor, tag or alias text after its indicator (`fm_property` in flow).
static bool flow_property(FlowCheck *f) {
    TSLexer *lexer = f->lexer;
    int32_t kind = lexer->lookahead;
    fm_advance(lexer);
    size_t length = 0;
    if (kind == '!' && lexer->lookahead == '<') {
        while (!fm_at_eol(lexer) && lexer->lookahead != '>') {
            fm_advance(lexer);
            length++;
        }
        if (lexer->lookahead == '>') fm_advance(lexer);
    }
    while (!fm_at_eol(lexer) && !is_blank(lexer->lookahead) &&
           !fm_is_flow_indicator(lexer->lookahead)) {
        fm_advance(lexer);
        length++;
    }
    return kind == '!' || length > 0;
}

// The rest of a flow plain scalar (`fm_flow_plain`): ends at a flow indicator,
// a `:` followed by whitespace / an indicator / the line end, ` #`, or the
// line end.  The scanner decides about a `:` by the character after it, which
// the checker cannot un-read, so a separating `:` is consumed and reported by
// returning true.
static bool flow_plain_rest(FlowCheck *f) {
    TSLexer *lexer = f->lexer;
    bool pending_ws = false;
    while (!fm_at_eol(lexer)) {
        int32_t c = lexer->lookahead;
        if (fm_is_flow_indicator(c)) return false;
        if (c == ':') {
            fm_advance(lexer);
            int32_t n = lexer->lookahead;
            if (is_blank(n) || fm_at_eol(lexer) || fm_is_flow_indicator(n)) return true;
            pending_ws = false;
            continue;
        }
        if (is_blank(c)) {
            fm_advance(lexer);
            pending_ws = true;
            continue;
        }
        if (c == '#' && pending_ws) return false;
        fm_advance(lexer);
        pending_ws = false;
    }
    return false;
}

static bool flow_collection(FlowCheck *f, int32_t close);

// props* scalar (`_flow_node`).  Returns 1 for a node, 2 for a node followed
// by a separating `:` that was consumed, 0 if the input does not fit.
static int flow_node(FlowCheck *f) {
    TSLexer *lexer = f->lexer;
    while (lexer->lookahead == '&' || lexer->lookahead == '!') {
        if (!flow_property(f)) return 0;
        if (!flow_skip(f, false)) return 0;
    }
    int32_t c = lexer->lookahead;
    if (c == '*') return flow_property(f) ? 1 : 0;
    if (c == '{' || c == '[') {
        int32_t close = c == '{' ? '}' : ']';
        fm_advance(lexer);
        return flow_collection(f, close) ? 1 : 0;
    }
    if (c == '"' || c == '\'') {
        fm_advance(lexer);
        for (;;) {
            if (lexer->eof(lexer)) return 0;
            int32_t q = lexer->lookahead;
            if (c == '"' && q == '\\') {
                fm_advance(lexer);
                if (is_newline(lexer->lookahead)) return 0; // out of subset
                if (!lexer->eof(lexer)) fm_advance(lexer);
                continue;
            }
            if (is_newline(q)) {
                int16_t indent = fm_consume_to_content(lexer);
                if (indent < 0) return 0;
                if (indent == 0 && (lexer->lookahead == '-' || lexer->lookahead == '.') &&
                    fm_document_marker(lexer, lexer->lookahead)) {
                    return 0;
                }
                continue;
            }
            fm_advance(lexer);
            if (q == c) {
                if (c == '\'' && lexer->lookahead == '\'') {
                    fm_advance(lexer);
                    continue;
                }
                return 1;
            }
        }
    }
    // A plain scalar, started the way `fm_token` starts one in flow context.
    if (fm_is_flow_indicator(c) || c == '#' || c == '|' || c == '>' || c == '%' ||
        c == '@' || c == '`' || c == ':' || fm_at_eol(lexer) || is_blank(c)) {
        return 0;
    }
    if (c == '-' || c == '?') {
        fm_advance(lexer);
        if (is_blank(lexer->lookahead) || fm_at_eol(lexer)) return 0;
    }
    return flow_plain_rest(f) ? 2 : 1;
}

// An entry (`_flow_entry`): a node, or a pair `node : [node]`.
static bool flow_entry(FlowCheck *f) {
    TSLexer *lexer = f->lexer;
    int node = flow_node(f);
    if (node == 0) return false;
    bool separator = node == 2;
    if (!separator) {
        if (!flow_skip(f, false)) return false;
        if (lexer->lookahead == ':') {
            // after a quoted scalar, alias or collection (JSON style), or
            // after whitespace
            fm_advance(lexer);
            separator = true;
        }
    }
    if (!separator) return true;
    if (!flow_skip(f, true)) return false;
    int32_t c = lexer->lookahead;
    if (c == ',' || c == '}' || c == ']') return true; // empty value
    int value = flow_node(f);
    return value == 1; // `a: b: c` is not in the grammar
}

// The contents of a collection whose opening bracket has been read.
static bool flow_collection(FlowCheck *f, int32_t close) {
    TSLexer *lexer = f->lexer;
    if (++f->depth > 32) return false;
    if (!flow_skip(f, true)) return false;
    bool first = true;
    for (;;) {
        int32_t c = lexer->lookahead;
        if (c == close) {
            fm_advance(lexer);
            f->depth--;
            return true;
        }
        if (!first) {
            if (c != ',') return false;
            fm_advance(lexer);
            if (!flow_skip(f, true)) return false;
            if (lexer->lookahead == close) continue; // trailing comma
        }
        if (!flow_entry(f)) return false;
        if (!flow_skip(f, false)) return false;
        first = false;
    }
}

// '{' / '['.  Opened only when the whole collection fits the flow grammar
// and, in block context, nothing but a comment follows it on its line.
static bool fm_flow_start(Scanner *s, TSLexer *lexer, const bool *valid_symbols,
                          bool line_start, bool in_flow) {
    TokenType token = lexer->lookahead == '{' ? FM_FLOW_MAPPING_START : FM_FLOW_SEQUENCE_START;
    int32_t close = lexer->lookahead == '{' ? '}' : ']';
    if (!valid_symbols[token]) {
        if (valid_symbols[FM_UNSUPPORTED]) return fm_unsupported_to_eol(s, lexer);
        return false;
    }
    // The token is the bracket alone; everything after it is lookahead.
    fm_advance(lexer);
    lexer->mark_end(lexer);
    // A nested collection was checked along with the outermost one.
    if (in_flow) return fm_emit(s, lexer, token, FM_MID_LINE);
    FlowCheck check = {lexer, fm_threshold(s, line_start), 0};
    bool ok = flow_collection(&check, close);
    if (ok && !in_flow) {
        bool after_space = false;
        while (is_blank(lexer->lookahead)) {
            fm_advance(lexer);
            after_space = true;
        }
        ok = fm_at_eol(lexer) || (lexer->lookahead == '#' && after_space);
    }
    if (ok) return fm_emit(s, lexer, token, FM_MID_LINE);
    if (in_flow || !valid_symbols[FM_UNSUPPORTED]) return false;
    // The token already ends after the bracket; the rest of the line follows
    // as `_fm_unsupported_rest`.
    s->fm_unsupported_rest = true;
    return fm_emit(s, lexer, FM_UNSUPPORTED, FM_MID_LINE);
}

// The token that starts at the cursor (mid-line, or at a line start whose
// structural token has been decided).
static bool fm_token(Scanner *s, TSLexer *lexer, const bool *valid_symbols) {
    // A node after anchors or tags stands where the first of them stands.
    bool at_line_start = s->fm_line != FM_MID_LINE;
    bool line_start = at_line_start || s->fm_node_line_start;
    bool in_flow = valid_symbols[FM_FLOW_PLAIN];
    bool at_column_0 = lexer->get_column(lexer) == 0;
    uint32_t column = (uint32_t)(s->fm_node_column >= 0 ? s->fm_node_column
                                                        : fm_column(s, lexer));
    int32_t c = lexer->lookahead;

    if (at_line_start && !s->fm_root_known) {
        s->fm_root_known = true;
        s->fm_levels[0].indent = (int16_t)column;
    }

    // `---` closes the frontmatter; `...` ends the YAML document.  Only at
    // column 0 (spec §4.2), and by then every block has been closed.
    if (at_line_start && at_column_0 && (c == '-' || c == '.')) {
        size_t run = 0;
        while (run < 3 && lexer->lookahead == c) {
            fm_advance(lexer);
            run++;
        }
        if (run == 3 && (is_blank(lexer->lookahead) || fm_at_eol(lexer))) {
            while (!fm_at_eol(lexer)) fm_advance(lexer);
            if (c == '.') {
                lexer->mark_end(lexer);
                return fm_emit(s, lexer, FM_DOCUMENT_END, FM_MID_LINE);
            }
            if (valid_symbols[FM_CLOSE]) {
                if (lexer->lookahead == '\r') fm_advance(lexer);
                if (lexer->lookahead == '\n') fm_advance(lexer);
                lexer->mark_end(lexer);
                fm_reset(s);
                s->indentation = 0;
                s->column = 0;
                lexer->result_symbol = FM_CLOSE;
                return true;
            }
            if (valid_symbols[FM_UNSUPPORTED]) {
                lexer->mark_end(lexer);
                return fm_emit(s, lexer, FM_UNSUPPORTED, FM_MID_LINE);
            }
            return false;
        }
        // Not a marker: the run read so far starts the line's token.
        bool dash = c == '-' && run == 1 && (is_blank(lexer->lookahead) || fm_at_eol(lexer));
        if (dash) {
            if (valid_symbols[FM_DASH]) {
                lexer->mark_end(lexer);
                return fm_emit(s, lexer, FM_DASH, FM_MID_LINE);
            }
            if (valid_symbols[FM_UNSUPPORTED]) return fm_unsupported_to_eol(s, lexer);
            return false;
        }
        if (in_flow) {
            lexer->mark_end(lexer);
            return fm_flow_plain(s, lexer) || fm_emit(s, lexer, FM_FLOW_PLAIN, FM_MID_LINE);
        }
        return fm_plain(s, lexer, valid_symbols, line_start, column, true);
    }

    switch (c) {
        case '#':
            while (!fm_at_eol(lexer)) fm_advance(lexer);
            lexer->mark_end(lexer);
            return fm_emit(s, lexer, FM_COMMENT, FM_MID_LINE);
        case '%':
            // a directive is a column-0 line (as `fm_peek_next_line` has it)
            if (at_line_start && at_column_0) {
                while (!fm_at_eol(lexer)) fm_advance(lexer);
                lexer->mark_end(lexer);
                return fm_emit(s, lexer, FM_DIRECTIVE, FM_MID_LINE);
            }
            break;
        case '-':
            fm_advance(lexer);
            if (is_blank(lexer->lookahead) || fm_at_eol(lexer)) {
                if (valid_symbols[FM_DASH] &&
                    (line_start || fm_push(s, (int16_t)column, FM_LEVEL_BLOCK))) {
                    lexer->mark_end(lexer);
                    return fm_emit(s, lexer, FM_DASH, FM_MID_LINE);
                }
                if (valid_symbols[FM_UNSUPPORTED]) return fm_unsupported_to_eol(s, lexer);
                return false;
            }
            if (in_flow) {
                lexer->mark_end(lexer);
                return fm_flow_plain(s, lexer) || fm_emit(s, lexer, FM_FLOW_PLAIN, FM_MID_LINE);
            }
            return fm_plain(s, lexer, valid_symbols, line_start, column, true);
        case '?':
        case ':': {
            // Where no node can start, `:` is the mapping value indicator (or
            // the flow `:`), an internal token.  Where one can, it starts an
            // empty-key entry (out of subset) or a plain scalar like `:x`.
            bool node_here = valid_symbols[FM_KEY] || valid_symbols[FM_PLAIN] ||
                             valid_symbols[FM_FLOW_PLAIN];
            // (A `:` that starts a line never is: it would follow a key.)
            if (c == ':' && !line_start && (!node_here || in_flow)) return false;
            fm_advance(lexer);
            if (is_blank(lexer->lookahead) || fm_at_eol(lexer)) {
                // explicit keys and empty keys are out of subset
                if (valid_symbols[FM_UNSUPPORTED]) return fm_unsupported_to_eol(s, lexer);
                return false;
            }
            if (in_flow) {
                lexer->mark_end(lexer);
                return fm_flow_plain(s, lexer) || fm_emit(s, lexer, FM_FLOW_PLAIN, FM_MID_LINE);
            }
            return fm_plain(s, lexer, valid_symbols, line_start, column, true);
        }
        case '&':
            return fm_property(s, lexer, valid_symbols, FM_ANCHOR, in_flow, (int16_t)column,
                               line_start);
        case '!':
            return fm_property(s, lexer, valid_symbols, FM_TAG, in_flow, (int16_t)column,
                               line_start);
        case '*':
            return fm_property(s, lexer, valid_symbols, FM_ALIAS, in_flow, (int16_t)column,
                               line_start);
        case '|':
        case '>':
            if (in_flow) break;
            if (valid_symbols[FM_BLOCK_SCALAR]) {
                return fm_block_scalar(s, lexer, valid_symbols, line_start);
            }
            if (valid_symbols[FM_UNSUPPORTED]) return fm_unsupported_to_eol(s, lexer);
            return false;
        case '\'':
        case '"':
            return fm_quoted(s, lexer, valid_symbols, line_start, column, in_flow);
        case '{':
        case '[':
            return fm_flow_start(s, lexer, valid_symbols, line_start, in_flow);
        case ',':
        case ']':
        case '}':
            if (in_flow) return false; // flow punctuation is an internal token
            break;
        case '@':
        case '`':
            break;
        default:
            if (in_flow) return fm_flow_plain(s, lexer);
            return fm_plain(s, lexer, valid_symbols, line_start, column, false);
    }
    if (valid_symbols[FM_UNSUPPORTED]) return fm_unsupported_to_eol(s, lexer);
    return false;
}

typedef enum {
    FM_NEXT_EOF,
    FM_NEXT_CLOSE,    // `---`
    FM_NEXT_DOC_END,  // `...`
    FM_NEXT_DASH,     // `- ` or a lone `-`
    FM_NEXT_OTHER,
} FmNextLine;

// From a line break, look ahead to the next line that carries structure:
// blank, comment and directive lines are passed over.  Reports that line's
// indentation and what it starts with.  Everything read is lookahead.
static FmNextLine fm_peek_next_line(TSLexer *lexer, int16_t *indent_out) {
    int16_t indent = 0;
    for (;;) {
        if (lexer->eof(lexer)) {
            *indent_out = 0;
            return FM_NEXT_EOF;
        }
        int32_t c = lexer->lookahead;
        if (is_newline(c)) {
            fm_advance(lexer);
            indent = 0;
            continue;
        }
        if (c == ' ') {
            fm_advance(lexer);
            indent++;
            continue;
        }
        if (c == '\t') {
            fm_advance(lexer);
            indent = (int16_t)((indent / FM_TAB_WIDTH + 1) * FM_TAB_WIDTH);
            continue;
        }
        if (c == '#' || (c == '%' && indent == 0)) {
            while (!fm_at_eol(lexer)) fm_advance(lexer);
            continue;
        }
        break;
    }
    *indent_out = indent;
    int32_t c = lexer->lookahead;
    if (c == '-' || c == '.') {
        size_t run = 0;
        size_t limit = indent == 0 ? 3 : 1;
        while (run < limit && lexer->lookahead == c) {
            fm_advance(lexer);
            run++;
        }
        bool followed_by_space = is_blank(lexer->lookahead) || fm_at_eol(lexer);
        if (run == 3 && followed_by_space) {
            return c == '-' ? FM_NEXT_CLOSE : FM_NEXT_DOC_END;
        }
        if (c == '-' && run == 1 && followed_by_space) return FM_NEXT_DASH;
    }
    return FM_NEXT_OTHER;
}

// At a line break (the cursor is on the line ending): decide the structure of
// the next line and emit it as a zero-width token right here, at the end of
// the previous line, so no node's range reaches into the next line.  The line
// ending and indentation follow as whitespace.
static bool fm_line_break(Scanner *s, TSLexer *lexer, const bool *valid_symbols) {
    lexer->mark_end(lexer);
    int16_t indent = 0;
    FmNextLine next = fm_peek_next_line(lexer, &indent);

    if (next != FM_NEXT_EOF && next != FM_NEXT_CLOSE && next != FM_NEXT_DOC_END &&
        !s->fm_root_known) {
        s->fm_root_known = true;
        s->fm_levels[0].indent = indent;
    }

    FmLevel *top = fm_top(s);
    bool closes_all = next == FM_NEXT_EOF || next == FM_NEXT_CLOSE || next == FM_NEXT_DOC_END;
    if (s->fm_depth > 1 && valid_symbols[FM_DEDENT] &&
        (closes_all || indent < top->indent ||
         (top->kind == FM_LEVEL_ZERO_SEQ && indent == top->indent &&
          next != FM_NEXT_DASH))) {
        // one level per token; the next call comes back to this line break
        s->fm_depth--;
        return fm_emit(s, lexer, FM_DEDENT, FM_MID_LINE);
    }
    if (!closes_all) {
        if (indent > top->indent) {
            if (valid_symbols[FM_INDENT] && fm_push(s, indent, FM_LEVEL_BLOCK)) {
                return fm_emit(s, lexer, FM_INDENT, FM_BREAK_PENDING);
            }
            if (valid_symbols[FM_NEWLINE]) {
                // Over-indented, but nothing here can own a nested block
                // (`a: b` then `  c: d`), or nesting is at FM_MAX_DEPTH: not
                // a sibling, so the line is opaque rather than mis-nested (P4).
                s->fm_force_unsupported = true;
                return fm_emit(s, lexer, FM_NEWLINE, FM_BREAK_PENDING);
            }
        } else {
            if (next == FM_NEXT_DASH && valid_symbols[FM_SEQUENCE_NEWLINE] &&
                fm_push(s, indent, FM_LEVEL_ZERO_SEQ)) {
                return fm_emit(s, lexer, FM_SEQUENCE_NEWLINE, FM_BREAK_PENDING);
            }
            if (valid_symbols[FM_NEWLINE]) {
                // Shallower than a block it cannot close (the root, when the
                // first line was indented): not a sibling either.
                if (indent < top->indent) s->fm_force_unsupported = true;
                return fm_emit(s, lexer, FM_NEWLINE, FM_BREAK_PENDING);
            }
        }
    }
    // No structural token applies (the first content line, a closing
    // delimiter, a flow collection's content, ...): a zero-width placeholder,
    // after which the whitespace is consumed as usual.
    return fm_emit(s, lexer, FM_SPACE, FM_BREAK_PENDING);
}

static bool fm_scan(Scanner *s, TSLexer *lexer, const bool *valid_symbols) {
    // The rest of a line whose first token was opaque.
    if (s->fm_unsupported_rest) {
        s->fm_unsupported_rest = false;
        if (valid_symbols[FM_UNSUPPORTED_REST] && !fm_at_eol(lexer)) {
            while (!fm_at_eol(lexer)) fm_advance(lexer);
            lexer->mark_end(lexer);
            return fm_emit(s, lexer, FM_UNSUPPORTED_REST, FM_MID_LINE);
        }
    }

    if (lexer->eof(lexer)) {
        // An unterminated block: close what is open, then let error recovery
        // insert the MISSING `---`.
        if (s->fm_depth > 1 && valid_symbols[FM_DEDENT]) {
            s->fm_depth--;
            return fm_emit(s, lexer, FM_DEDENT, FM_MID_LINE);
        }
        return false;
    }

    int32_t c = lexer->lookahead;
    if (is_newline(c)) {
        if (s->fm_line == FM_BREAK_PENDING) {
            // the line ending, blank lines and the indentation of the next
            // line whose structure was already decided
            while (is_newline(lexer->lookahead)) {
                fm_advance(lexer);
                fm_skip_indentation(s, lexer);
            }
            lexer->mark_end(lexer);
            bool more_pending = lexer->lookahead == '#' ||
                                (lexer->lookahead == '%' && s->fm_line_chars == 0) ||
                                lexer->eof(lexer);
            return fm_emit(s, lexer, FM_SPACE,
                           more_pending ? FM_BREAK_PENDING : FM_LINE_START);
        }
        bool structural = valid_symbols[FM_INDENT] || valid_symbols[FM_NEWLINE] ||
                          valid_symbols[FM_DEDENT] || valid_symbols[FM_SEQUENCE_NEWLINE];
        if (!structural && valid_symbols[FM_FLOW_PLAIN]) {
            // Inside a flow collection line breaks are whitespace.
            while (is_newline(lexer->lookahead)) {
                fm_advance(lexer);
                fm_skip_indentation(s, lexer);
            }
            lexer->mark_end(lexer);
            return fm_emit(s, lexer, FM_SPACE, FM_MID_LINE);
        }
        return fm_line_break(s, lexer, valid_symbols);
    }

    if (s->fm_line == FM_BREAK_PENDING) {
        // A comment or directive line before the line the structure was
        // decided for.  (A directive only at column 0, as in
        // `fm_peek_next_line`: an indented `%` is the decided line itself.)
        if (c == '#' || (c == '%' && lexer->get_column(lexer) == 0)) {
            while (!fm_at_eol(lexer)) fm_advance(lexer);
            lexer->mark_end(lexer);
            return fm_emit(s, lexer, c == '#' ? FM_COMMENT : FM_DIRECTIVE,
                           FM_BREAK_PENDING);
        }
        s->fm_line = FM_LINE_START;
    }

    if (s->fm_force_unsupported) {
        s->fm_force_unsupported = false;
        if (valid_symbols[FM_UNSUPPORTED]) return fm_unsupported_to_eol(s, lexer);
    }

    if (is_blank(c)) {
        if (s->fm_line != FM_MID_LINE) {
            // the first line's indentation (later lines' is consumed with
            // their line break)
            fm_skip_indentation(s, lexer);
        } else {
            while (is_blank(lexer->lookahead)) fm_advance(lexer);
        }
        lexer->mark_end(lexer);
        lexer->result_symbol = FM_SPACE;
        return true; // whitespace keeps the line position
    }

    return fm_token(s, lexer, valid_symbols);
}

/* ========================================================================
 * Entry points
 * ======================================================================== */

static bool scan(Scanner *s, TSLexer *lexer, const bool *valid_symbols) {
    // A grammar rule decided that the current branch is invalid.  During
    // error recovery every symbol is valid, which also lands here: the zero
    // width error token is then discarded and the internal lexer takes over.
    if (valid_symbols[TRIGGER_ERROR]) {
        return error(lexer);
    }
    if (s->fm_active) {
        return fm_scan(s, lexer, valid_symbols);
    }
    return md_scan(s, lexer, valid_symbols);
}

void *tree_sitter_okf_external_scanner_create(void) {
    init_paragraph_interrupt_symbols();
    Scanner *s = (Scanner *)ts_calloc(1, sizeof(Scanner));
    s->open_blocks.items = (Block *)ts_calloc(1, sizeof(Block));
    s->open_blocks.capacity = 1;
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
    _Static_assert(ATX_H6_MARKER == ATX_H1_MARKER + 5, "");
    _Static_assert(TOKEN_TYPE_COUNT == 126, "externals out of sync with grammar.js");
    _Static_assert(sizeof(PUNCTUATION) - 1 == PUNCTUATION_LAST - PUNCTUATION_FIRST + 1, "");
#else
    assert(ATX_H6_MARKER == ATX_H1_MARKER + 5);
#endif
    deserialize(s, NULL, 0);
    return s;
}

bool tree_sitter_okf_external_scanner_scan(void *payload, TSLexer *lexer,
                                           const bool *valid_symbols) {
    Scanner *scanner = (Scanner *)payload;
    scanner->simulate = false;
    scanner->pending_class = CLASS_OTHER;
    scanner->pending_column = 0;
    bool found = scan(scanner, lexer, valid_symbols);
    if (found) {
        // Every token resets what precedes the next position; the few that
        // can precede an emphasis delimiter run say what they were.
        scanner->prev_class = scanner->pending_class;
        scanner->prev_column = scanner->pending_column;
        // Keep the state canonical, so parse versions that reach the same
        // place with the same meaning also have the same bytes and merge (see
        // `serialize`): a delimiter run is over once any other token is
        // emitted, and block indentation means nothing after inline content.
        TSSymbol symbol = lexer->result_symbol;
        bool delimiter = symbol == EMPHASIS_OPEN_STAR || symbol == EMPHASIS_CLOSE_STAR ||
                         symbol == EMPHASIS_OPEN_UNDERSCORE ||
                         symbol == EMPHASIS_CLOSE_UNDERSCORE ||
                         symbol == STRIKETHROUGH_OPEN || symbol == STRIKETHROUGH_CLOSE;
        // (a literal run's delimiters are punctuation tokens that continue it)
        bool literal_run = (scanner->inline_state & STATE_LITERAL_RUN) &&
                           symbol >= PUNCTUATION_FIRST && symbol <= PUNCTUATION_LAST;
        if (!delimiter && !literal_run) {
            scanner->num_emphasis_delimiters_left = 0;
            scanner->inline_state &= ~(STATE_EMPHASIS_DELIMITER_IS_OPEN | STATE_LITERAL_RUN);
        }
        bool inline_token = delimiter || symbol == CODE_SPAN_START || symbol == CODE_SPAN_CLOSE ||
                            symbol == UNCLOSED_SPAN || symbol == FOOTNOTE_REFERENCE_START ||
                            symbol == FOOTNOTE_LABEL || symbol == FOOTNOTE_REFERENCE_END ||
                            symbol == LITERAL_OPEN_BRACKET || symbol == WHITESPACE_GE_2 ||
                            symbol == WHITESPACE_1 || symbol == PIPE_TABLE_PIPE ||
                            symbol == PIPE_TABLE_CELL_LEADING_SPACE ||
                            symbol == PIPE_TABLE_CELL_TRAILING_SPACE ||
                            symbol == PIPE_TABLE_EMPTY_CELL ||
                            (symbol >= PUNCTUATION_FIRST && symbol <= PUNCTUATION_LAST);
        if (inline_token && !scanner->fm_active) scanner->indentation = 0;
    }
    return found;
}

unsigned tree_sitter_okf_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *scanner = (Scanner *)payload;
    return serialize(scanner, buffer);
}

void tree_sitter_okf_external_scanner_deserialize(void *payload, const char *buffer,
                                                  unsigned length) {
    Scanner *scanner = (Scanner *)payload;
    deserialize(scanner, buffer, length);
}

void tree_sitter_okf_external_scanner_destroy(void *payload) {
    Scanner *scanner = (Scanner *)payload;
    ts_free(scanner->open_blocks.items);
    ts_free(scanner);
}
