; Per-claim attribution (spec §7.3, OKF §5.1).
;
; A body footnote whose label is a `sources[].id` attributes the claim it is
; attached to.  These capture both sides of that join; queries/okf/sources.scm
; captures the frontmatter side.  Whether every label resolves to a source is
; host code (spec §7.5).
;
;   @okf.citation.reference   the label of `[^id]` in the text
;   @okf.citation.definition  the label of `[^id]: …`
;   @okf.citation             the whole reference or definition

(footnote_reference
  label: (footnote_label) @okf.citation.reference) @okf.citation

(footnote_definition
  label: (footnote_label) @okf.citation.definition) @okf.citation
