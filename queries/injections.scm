; Injections for OKF documents.
;
; The frontmatter is NOT injected by default: it is parsed natively, so every
; OKF query works with this grammar alone (spec P5).  Hosts that want full
; YAML fidelity can use queries/injections-fullyaml.scm instead (spec D2).

; Fenced code: the info string's language names the grammar (`sql` for the
; `# Computation` payload, OKF §10.3).
(fenced_code_block
  (info_string
    (language) @injection.language)
  (code_fence_content) @injection.content)

((html_block) @injection.content
  (#set! injection.language "html"))

((html_tag) @injection.content
  (#set! injection.language "html"))
