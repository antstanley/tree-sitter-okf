; Tags for code navigation (spec §7.3).
;
; The concept a document defines is named by its frontmatter `title`; its
; sections are definitions too, so an outline lists them.  Cross-links are
; references to the concepts they point at: the name is the link destination,
; which a host resolves (spec §7.5 — resolution is not a query).

(source_file
  (frontmatter
    (block_mapping
      (block_mapping_pair
        key: (_) @_key
        value: (_) @name)))
  (#match? @_key "^[\"']?title[\"']?$")) @definition.class

(section
  (atx_heading
    heading_content: (inline) @name)) @definition.module

(inline_link
  (link_destination) @name) @reference.class

(full_reference_link
  (link_label) @name) @reference.class

(footnote_reference
  label: (footnote_label) @name) @reference.call
