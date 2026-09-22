; Highlighting for OKF documents.
;
; Capture names follow the common `@markup.*` convention (Neovim 0.10+,
; Helix).  The body rules are adapted from tree-sitter-markdown's block and
; inline highlight queries (MIT, via nvim-treesitter).

; --- frontmatter (OKF-YAML) --------------------------------------------------

(frontmatter
  "---" @punctuation.special)

(block_mapping_pair
  key: (_) @property)

(flow_pair
  key: (_) @property)

(plain_scalar) @string

[
  (single_quote_scalar)
  (double_quote_scalar)
] @string

(block_scalar) @string

[
  (anchor)
  (alias)
] @label

(tag) @type

(comment) @comment

[
  (yaml_directive)
  (yaml_document_end)
] @keyword.directive

(yaml_unsupported) @none

(block_sequence_item
  "-" @punctuation.delimiter)

[
  ":"
  ","
] @punctuation.delimiter

(flow_mapping
  [
    "{"
    "}"
  ] @punctuation.bracket)

(flow_sequence
  [
    "["
    "]"
  ] @punctuation.bracket)

; Scalar typing is a query policy, not a node (spec D3).  These mirror the
; conservative rules of queries/okf/scalars.scm.
((plain_scalar) @number
  (#match? @number "^[-+]?[0-9][0-9_]*(\\.[0-9_]*)?([eE][-+]?[0-9]+)?$"))

((plain_scalar) @boolean
  (#match? @boolean "^(true|True|TRUE|false|False|FALSE)$"))

((plain_scalar) @constant.builtin
  (#match? @constant.builtin "^(null|Null|NULL|~)$"))

; --- body: blocks -------------------------------------------------------------

(atx_heading
  (inline) @markup.heading)

(setext_heading
  (paragraph) @markup.heading)

(atx_heading
  (atx_h1_marker)) @markup.heading.1

(atx_heading
  (atx_h2_marker)) @markup.heading.2

(atx_heading
  (atx_h3_marker)) @markup.heading.3

(atx_heading
  (atx_h4_marker)) @markup.heading.4

(atx_heading
  (atx_h5_marker)) @markup.heading.5

(atx_heading
  (atx_h6_marker)) @markup.heading.6

[
  (atx_h1_marker)
  (atx_h2_marker)
  (atx_h3_marker)
  (atx_h4_marker)
  (atx_h5_marker)
  (atx_h6_marker)
  (setext_h1_underline)
  (setext_h2_underline)
] @punctuation.special

[
  (indented_code_block)
  (fenced_code_block)
] @markup.raw.block

(fenced_code_block_delimiter) @punctuation.delimiter

(info_string
  (language) @label)

(code_fence_content) @none

[
  (list_marker_plus)
  (list_marker_minus)
  (list_marker_star)
  (list_marker_dot)
  (list_marker_parenthesis)
] @markup.list

[
  (task_list_marker_checked)
  (task_list_marker_unchecked)
] @markup.list.checked

(task_list_marker_unchecked) @markup.list.unchecked

(thematic_break) @punctuation.special

(block_quote) @markup.quote

[
  (block_continuation)
  (block_quote_marker)
] @punctuation.special

(pipe_table_header
  (pipe_table_cell) @markup.heading)

(pipe_table_header
  "|" @punctuation.special)

(pipe_table_row
  "|" @punctuation.special)

(pipe_table_delimiter_row) @punctuation.special

(link_reference_definition
  (link_label) @markup.link.label)

(footnote_definition
  "[" @punctuation.delimiter
  "^" @punctuation.delimiter
  label: (footnote_label) @markup.link.label
  "]:" @punctuation.delimiter)

(html_block) @markup.raw.block

; --- body: inlines ------------------------------------------------------------

(code_span) @markup.raw

[
  (emphasis_delimiter)
  (code_span_delimiter)
] @punctuation.delimiter

(emphasis) @markup.italic

(strong_emphasis) @markup.strong

(strikethrough) @markup.strikethrough

[
  (link_destination)
  (uri_autolink)
  (email_autolink)
] @markup.link.url

[
  (link_label)
  (link_text)
  (image_description)
] @markup.link.label

(link_title) @string

(footnote_reference
  label: (footnote_label) @markup.link.label)

(footnote_reference
  [
    "["
    "^"
    "]"
  ] @punctuation.delimiter)

[
  (backslash_escape)
  (hard_line_break)
] @string.escape

[
  (entity_reference)
  (numeric_character_reference)
] @character.special

(html_tag) @tag

(image
  [
    "!"
    "["
    "]"
    "("
    ")"
  ] @punctuation.delimiter)

(inline_link
  [
    "["
    "]"
    "("
    ")"
  ] @punctuation.delimiter)

(shortcut_link
  [
    "["
    "]"
  ] @punctuation.delimiter)
