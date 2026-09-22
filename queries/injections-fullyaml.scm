; Opt-in preset (spec D2): hand the frontmatter to tree-sitter-yaml as well.
;
; Use this INSTEAD of queries/injections.scm when a host needs full YAML
; fidelity (anchors resolved, every YAML 1.2 construct).  The native OKF-YAML
; nodes stay in the tree either way, so queries/okf/*.scm keep working.
;
; Every child of the frontmatter between the delimiters is combined into one
; YAML document.

(frontmatter
  (_) @injection.content
  (#set! injection.language "yaml")
  (#set! injection.combined))

(fenced_code_block
  (info_string
    (language) @injection.language)
  (code_fence_content) @injection.content)

((html_block) @injection.content
  (#set! injection.language "html"))

((html_tag) @injection.content
  (#set! injection.language "html"))
