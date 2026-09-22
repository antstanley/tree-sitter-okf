; Scalar typing policy (spec D3).
;
; The grammar never resolves a scalar's type: `2026-12-31T00:00:00Z` and `0.2`
; are plain scalars, byte-faithful.  This file is the replaceable policy that
; says what a plain scalar LOOKS like.  It follows YAML 1.2's core schema,
; which is why `yes`/`no` are NOT booleans here, and it never touches quoted
; scalars: OKF §12 writes `okf_version: "0.2"` precisely so that it stays a
; string.  Replace or extend it per host; nothing in the grammar depends on it.

((plain_scalar) @okf.scalar.null
  (#match? @okf.scalar.null "^(null|Null|NULL|~)$"))

((plain_scalar) @okf.scalar.boolean
  (#match? @okf.scalar.boolean "^(true|True|TRUE|false|False|FALSE)$"))

((plain_scalar) @okf.scalar.integer
  (#match? @okf.scalar.integer "^([-+]?[0-9]+|0o[0-7]+|0x[0-9a-fA-F]+)$"))

((plain_scalar) @okf.scalar.float
  (#match? @okf.scalar.float "^([-+]?([.][0-9]+|[0-9]+([.][0-9]*)?)([eE][-+]?[0-9]+)?|[-+]?[.](inf|Inf|INF)|[.](nan|NaN|NAN))$")
  (#not-match? @okf.scalar.float "^[-+]?[0-9]+$"))

; ISO 8601 dates and timestamps, as used by `generated.at`, `stale_after`,
; `last_modified` (OKF §5).
((plain_scalar) @okf.scalar.timestamp
  (#match? @okf.scalar.timestamp "^[0-9]{4}-[0-9]{2}-[0-9]{2}([Tt ][0-9]{1,2}:[0-9]{2}:[0-9]{2}([.][0-9]+)?([ ]*(Z|[-+][0-9]{1,2}(:[0-9]{2})?))?)?$"))

((single_quote_scalar) @okf.scalar.timestamp
  (#match? @okf.scalar.timestamp "^'[0-9]{4}-[0-9]{2}-[0-9]{2}([Tt ][0-9]{1,2}:[0-9]{2}:[0-9]{2}([.][0-9]+)?([ ]*(Z|[-+][0-9]{1,2}(:[0-9]{2})?))?)?'$"))
