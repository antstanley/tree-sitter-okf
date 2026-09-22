; Well-known OKF frontmatter fields (spec §7.3, OKF §4.1).
;
; One named capture per key, `@okf.field.<key>`, on the pair's VALUE, plus
; `@okf.field` on the whole pair.  Only top-level pairs of the frontmatter
; mapping match: a nested `title:` inside `sources:` is not the document's
; title.  Quoted keys (`"type": X`) match too.
;
; The key set is versioned with the query library (it targets OKF v0.2), not
; with the parser: new keys are added here, never as grammar nodes (spec D5).
; An absent key simply produces no capture; "is `type` present?" is host code
; (spec §7.5).
;
; Only node patterns and #match? are used, so every host evaluates these the
; same way.

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_key
      value: (_) @okf.field.type) @okf.field)
  (#match? @_key "^[\"']?type[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_key
      value: (_) @okf.field.title) @okf.field)
  (#match? @_key "^[\"']?title[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_key
      value: (_) @okf.field.description) @okf.field)
  (#match? @_key "^[\"']?description[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_key
      value: (_) @okf.field.resource) @okf.field)
  (#match? @_key "^[\"']?resource[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_key
      value: (_) @okf.field.tags) @okf.field)
  (#match? @_key "^[\"']?tags[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_key
      value: (_) @okf.field.generated) @okf.field)
  (#match? @_key "^[\"']?generated[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_key
      value: (_) @okf.field.verified) @okf.field)
  (#match? @_key "^[\"']?verified[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_key
      value: (_) @okf.field.status) @okf.field)
  (#match? @_key "^[\"']?status[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_key
      value: (_) @okf.field.stale_after) @okf.field)
  (#match? @_key "^[\"']?stale_after[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_key
      value: (_) @okf.field.sources) @okf.field)
  (#match? @_key "^[\"']?sources[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_key
      value: (_) @okf.field.usage_window) @okf.field)
  (#match? @_key "^[\"']?usage_window[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_key
      value: (_) @okf.field.runtime) @okf.field)
  (#match? @_key "^[\"']?runtime[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_key
      value: (_) @okf.field.parameters) @okf.field)
  (#match? @_key "^[\"']?parameters[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_key
      value: (_) @okf.field.computation) @okf.field)
  (#match? @_key "^[\"']?computation[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_key
      value: (_) @okf.field.executor) @okf.field)
  (#match? @_key "^[\"']?executor[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_key
      value: (_) @okf.field.attester) @okf.field)
  (#match? @_key "^[\"']?attester[\"']?$"))

(frontmatter
  (block_mapping
    (block_mapping_pair
      key: (_) @_key
      value: (_) @okf.field.okf_version) @okf.field)
  (#match? @_key "^[\"']?okf_version[\"']?$"))
