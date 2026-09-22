; Conventional OKF headings (spec §6.3, OKF §4.2, OKF §9).
;
; They are ordinary headings in the tree; this file names them.  `@okf.section`
; captures the section a heading opens (the heading and everything up to the
; next heading of the same or higher level) and `@okf.section.heading` its
; text.  Matching is case-insensitive and allows trailing whitespace.  The set
; grows with the spec; add to it here, never to the grammar.

((section
  (atx_heading
    heading_content: (inline) @okf.section.heading)) @okf.section.schema
  (#match? @okf.section.heading "^[Ss][Cc][Hh][Ee][Mm][Aa][ \t]*$"))

((section
  (atx_heading
    heading_content: (inline) @okf.section.heading)) @okf.section.examples
  (#match? @okf.section.heading "^[Ee][Xx][Aa][Mm][Pp][Ll][Ee][Ss][ \t]*$"))

((section
  (atx_heading
    heading_content: (inline) @okf.section.heading)) @okf.section.computation
  (#match? @okf.section.heading "^[Cc][Oo][Mm][Pp][Uu][Tt][Aa][Tt][Ii][Oo][Nn][ \t]*$"))

((section
  (atx_heading
    heading_content: (inline) @okf.section.heading)) @okf.section.citations
  (#match? @okf.section.heading "^[Cc][Ii][Tt][Aa][Tt][Ii][Oo][Nn][Ss][ \t]*$"))

; log.md: one `## YYYY-MM-DD` section per day (OKF §9).
((section
  (atx_heading
    (atx_h2_marker)
    heading_content: (inline) @okf.log.date)) @okf.log.entry
  (#match? @okf.log.date "^[0-9]{4}-[0-9]{2}-[0-9]{2}[ \t]*$"))
