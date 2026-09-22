# tree-sitter-okf

A tree-sitter grammar for [Open Knowledge Format (OKF)](https://github.com/GoogleCloudPlatform/open-knowledge-format) v0.2 — markdown files with YAML frontmatter, organised into knowledge bundles.

**Status: pre-implementation.** There is no grammar yet. The design is at the review stage:

> ### 📖 [Read the grammar specification → `docs/grammar-spec.md`](docs/grammar-spec.md)
>
> Draft v0.1, **open for review**. It defines what the grammar will parse, the
> node types it will emit, the query library it will ship, and the twelve
> consequential design decisions (**D1–D12**) plus fifteen open questions
> (**Q1–Q15**) that need a human to weigh in — starting with **D2** (frontmatter)
> and **D4** (markdown fidelity), which together decide ~80% of the cost.

## Why does this need its own grammar?

`.md` files in an OKF bundle are not just markdown. Three things make them a format:

1. **The document shape is OKF's** — a frontmatter block plus a body, with a
   small but real set of rules about where frontmatter is and isn't allowed.
2. **The profile is OKF's** — GFM tables (for `# Schema`), footnote definitions
   and references (for per-claim attribution), fenced *and* indented code blocks,
   and cross-links in three path forms. `tree-sitter-markdown` ships none of the
   footnote support and gates its GFM support behind compile-time flags.
3. **The semantic join spans the frontmatter/body boundary** — `sources[].id` in
   frontmatter is joined to footnote labels in the body. No existing grammar can
   express that in a single query.

Every OKF tool in the [ecosystem](https://openknowledgeformat.com/ecosystem)
currently hand-rolls frontmatter scanning. One grammar plus one query library
replaces all of that, and buys editors highlighting, folding, structure outlines
and code injections for free.

## Repo layout

```
docs/grammar-spec.md   the design, for review — start here
script/corpus-stats    reproduces every number in the spec's Appendix A
```

The rest (`grammar.js`, `queries/`, `test/`, bindings) lands per the milestone
plan in §12 of the spec.

## Reproducing the corpus evidence

Appendix A of the spec claims specific things about what real OKF files contain.
Those claims are not vibes — they are the output of a committed script:

```bash
script/corpus-stats path/to/a/bundle
```

Against the four official sample bundles at upstream commit `ad30107`, it
reports 78 markdown files (54 with frontmatter), 484 plain scalars, 140 block
sequence items of which 129 sit at *zero indentation* relative to their key,
21 flow mappings, zero anchors, zero block scalars, and zero comments — plus the
body-side counts that drive the markdown-fidelity decision.

## License

TBD — the upstream OKF specification is Apache-2.0, and the vendored
`tree-sitter-markdown` rules are MIT; both need attribution if the fixture
bundles are vendored per §9.2.
