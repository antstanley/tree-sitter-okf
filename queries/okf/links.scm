; Link classification (spec §7.4, OKF §6.1–6.2).
;
; Every link destination, image source and link reference definition target is
; captured once as `@okf.link` and once by its form:
;
;   @okf.link.uri              has a scheme              https://…, mailto:…
;   @okf.link.bundle_relative  starts with `/`           /metrics/revenue.md  (the recommended form)
;   @okf.link.fragment         starts with `#`           #schema
;   @okf.link.relative         anything else             ./x.md, ../t/y.md, revenue.md
;
; These classify a PATH-SHAPED STRING.  A capture never means "resolvable":
; broken links are conformant (OKF §6.1, §11), and resolution needs the bundle
; (spec §7.5).

([
  (inline_link (link_destination) @okf.link)
  (image (link_destination) @okf.link)
  (link_reference_definition (link_destination) @okf.link)
])

([
  (inline_link (link_destination) @okf.link.uri)
  (image (link_destination) @okf.link.uri)
  (link_reference_definition (link_destination) @okf.link.uri)
]
  (#match? @okf.link.uri "^<?[A-Za-z][A-Za-z0-9+.-]*:"))

([
  (inline_link (link_destination) @okf.link.bundle_relative)
  (image (link_destination) @okf.link.bundle_relative)
  (link_reference_definition (link_destination) @okf.link.bundle_relative)
]
  (#match? @okf.link.bundle_relative "^<?/"))

([
  (inline_link (link_destination) @okf.link.fragment)
  (image (link_destination) @okf.link.fragment)
  (link_reference_definition (link_destination) @okf.link.fragment)
]
  (#match? @okf.link.fragment "^<?#"))

([
  (inline_link (link_destination) @okf.link.relative)
  (image (link_destination) @okf.link.relative)
  (link_reference_definition (link_destination) @okf.link.relative)
]
  (#match? @okf.link.relative "^<?([^/#:<]|[.])")
  (#not-match? @okf.link.relative "^<?[A-Za-z][A-Za-z0-9+.-]*:"))

; Autolinks are always URIs.
[
  (uri_autolink)
  (email_autolink)
] @okf.link

[
  (uri_autolink)
  (email_autolink)
] @okf.link.uri
