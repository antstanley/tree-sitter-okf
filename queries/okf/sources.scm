; Frontmatter `sources[]` entries (spec §7.3, OKF §5.1).
;
; `@okf.source` is one entry of the top-level `sources:` sequence and
; `@okf.source.<field>` the value of one of its fields.  Both block
; (`- id: x`) and flow (`- { id: x }`) entries match.  `sources[].id` is the
; key the body's footnote labels join against (queries/okf/citations.scm).
;
; `resource` may be a path OR a scope descriptor such as "all queries in
; BigQuery project X" (OKF §5.1): a capture says nothing about resolvability.

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_sources
      value: (block_sequence
        (block_sequence_item
          (block_mapping
            (block_mapping_pair
              key: (_) @_field
              value: (_) @okf.source.id))) @okf.source)))
  (#match? @_sources "^[\"']?sources[\"']?$")
  (#match? @_field "^[\"']?id[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_sources
      value: (block_sequence
        (block_sequence_item
          (flow_mapping
            (flow_pair
              key: (_) @_field
              value: (_) @okf.source.id))) @okf.source)))
  (#match? @_sources "^[\"']?sources[\"']?$")
  (#match? @_field "^[\"']?id[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_sources
      value: (block_sequence
        (block_sequence_item
          (block_mapping
            (block_mapping_pair
              key: (_) @_field
              value: (_) @okf.source.resource))) @okf.source)))
  (#match? @_sources "^[\"']?sources[\"']?$")
  (#match? @_field "^[\"']?resource[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_sources
      value: (block_sequence
        (block_sequence_item
          (flow_mapping
            (flow_pair
              key: (_) @_field
              value: (_) @okf.source.resource))) @okf.source)))
  (#match? @_sources "^[\"']?sources[\"']?$")
  (#match? @_field "^[\"']?resource[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_sources
      value: (block_sequence
        (block_sequence_item
          (block_mapping
            (block_mapping_pair
              key: (_) @_field
              value: (_) @okf.source.title))) @okf.source)))
  (#match? @_sources "^[\"']?sources[\"']?$")
  (#match? @_field "^[\"']?title[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_sources
      value: (block_sequence
        (block_sequence_item
          (flow_mapping
            (flow_pair
              key: (_) @_field
              value: (_) @okf.source.title))) @okf.source)))
  (#match? @_sources "^[\"']?sources[\"']?$")
  (#match? @_field "^[\"']?title[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_sources
      value: (block_sequence
        (block_sequence_item
          (block_mapping
            (block_mapping_pair
              key: (_) @_field
              value: (_) @okf.source.author))) @okf.source)))
  (#match? @_sources "^[\"']?sources[\"']?$")
  (#match? @_field "^[\"']?author[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_sources
      value: (block_sequence
        (block_sequence_item
          (flow_mapping
            (flow_pair
              key: (_) @_field
              value: (_) @okf.source.author))) @okf.source)))
  (#match? @_sources "^[\"']?sources[\"']?$")
  (#match? @_field "^[\"']?author[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_sources
      value: (block_sequence
        (block_sequence_item
          (block_mapping
            (block_mapping_pair
              key: (_) @_field
              value: (_) @okf.source.usage_count))) @okf.source)))
  (#match? @_sources "^[\"']?sources[\"']?$")
  (#match? @_field "^[\"']?usage_count[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_sources
      value: (block_sequence
        (block_sequence_item
          (flow_mapping
            (flow_pair
              key: (_) @_field
              value: (_) @okf.source.usage_count))) @okf.source)))
  (#match? @_sources "^[\"']?sources[\"']?$")
  (#match? @_field "^[\"']?usage_count[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_sources
      value: (block_sequence
        (block_sequence_item
          (block_mapping
            (block_mapping_pair
              key: (_) @_field
              value: (_) @okf.source.last_modified))) @okf.source)))
  (#match? @_sources "^[\"']?sources[\"']?$")
  (#match? @_field "^[\"']?last_modified[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_sources
      value: (block_sequence
        (block_sequence_item
          (flow_mapping
            (flow_pair
              key: (_) @_field
              value: (_) @okf.source.last_modified))) @okf.source)))
  (#match? @_sources "^[\"']?sources[\"']?$")
  (#match? @_field "^[\"']?last_modified[\"']?$"))
