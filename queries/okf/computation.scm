; The `# Computation` payload (spec §6.3, OKF §10.2–10.3).
;
; An Attested Computation's sanctioned code is the fenced or indented code
; block under its `# Computation` heading, before any subheading.
;
;   @okf.computation           the code block
;   @okf.computation.language  a fenced block's language (`sql`, OKF §10.3)
;   @okf.computation.code      the code itself
;
; "Exactly one such block" is a conformance rule, so it is host code
; (spec §7.5): a query can only report every candidate.

((section
  (atx_heading
    heading_content: (inline) @_heading)
  (fenced_code_block
    (info_string
      (language) @okf.computation.language)?
    (code_fence_content) @okf.computation.code) @okf.computation)
  (#match? @_heading "^[Cc][Oo][Mm][Pp][Uu][Tt][Aa][Tt][Ii][Oo][Nn][ \t]*$"))

((section
  (atx_heading
    heading_content: (inline) @_heading)
  (indented_code_block) @okf.computation @okf.computation.code)
  (#match? @_heading "^[Cc][Oo][Mm][Pp][Uu][Tt][Aa][Tt][Ii][Oo][Nn][ \t]*$"))
