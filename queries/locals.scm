; Scopes for structure outlines and region folding (spec §7.3).
;
; A section (a heading plus everything up to the next heading of the same or
; higher level) is a scope, and its heading's text defines it.  Footnote
; definitions and link reference definitions are definitions; their uses are
; references, so "go to definition" works on `[^id]` and `[label]`.

(section) @local.scope

(atx_heading
  heading_content: (inline) @local.definition)

(footnote_definition
  label: (footnote_label) @local.definition)

(footnote_reference
  label: (footnote_label) @local.reference)

(link_reference_definition
  (link_label) @local.definition)

(full_reference_link
  (link_label) @local.reference)
