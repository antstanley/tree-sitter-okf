# `tree-sitter-okf` — Grammar Specification

| | |
|---|---|
| **Document status** | Draft v0.1 — **for review** |
| **Target format** | Open Knowledge Format (OKF) v0.2 |
| **Upstream spec** | [GoogleCloudPlatform/open-knowledge-format `SPEC.md`](https://github.com/GoogleCloudPlatform/open-knowledge-format/blob/main/SPEC.md) — Apache-2.0, pinned at [`ad30107`](https://github.com/GoogleCloudPlatform/open-knowledge-format/commit/ad30107c31c06aec8a7d5636e0d1058118604e6f) |
| **Target toolchain** | tree-sitter CLI ≥ 0.25 (0.27.0 current), JSON ABI 14/15 |
| **Grammar name** | `okf` (language fn `tree_sitter_okf`) |
| **File types** | `*.md`, `*.markdown` |

---

## 0. How to review this document

This is a **design spec**, not the upstream OKF spec. It says *what the grammar will parse, what nodes it will emit, and which decisions are consequential* — before any `grammar.js` is written.

Review convention:

* Every consequential choice is a numbered decision **D1…D12** in §2, each with options, a recommendation, and the cost of getting it wrong.
* Anything I could not settle from evidence is a numbered question **Q1…Q15** in §11.
* Claims about the OKF corpus are backed by the measurement in **Appendix A** (run against the four official sample bundles).
* **Reference convention:** a bare `§N` means *this document*. The upstream OKF specification is always cited as `OKF §N` — the two documents reuse the same section numbers, so the prefix is load-bearing.
* Normative language here (`MUST`/`SHOULD`/`MAY`) is scoped **to this grammar**, not to OKF.

What would help most from a reviewer, in priority order:

1. **D2** (how to parse frontmatter) and **D4** (markdown fidelity strategy). These two decide ~80% of the implementation cost.
2. **D5** (what belongs in the grammar vs. in queries) — this decides the public API surface.
3. §11 questions — especially Q1 (scope: single document vs. bundle) and Q2 (who is the consumer).

Everything else is downstream of those.

---

## 1. Purpose and scope

### 1.1 What this grammar is

A single tree-sitter grammar that parses **one OKF document** — an OKF *concept document* (`<concept>.md`), an `index.md`, or a `log.md` — into one tree spanning both the YAML frontmatter and the markdown body.

The three things that make this grammar *OKF's* grammar rather than "markdown plus YAML":

1. **The document shape is OKF's.** OKF defines a document as *frontmatter block + body*, with a frontmatter block required for conformance (OKF §11.1–11.2) and forbidden in `index.md` except for the bundle-root `okf_version` key (OKF §8, OKF §12).
2. **The profile is OKF's.** OKF §4.2, §5.1, §6, §8 and §9 require specific construct families — fenced *and* indented code blocks, pipe tables, lists, cross-links in three path forms, GFM-style footnote definitions and references, and file-path-valued fields. The grammar commits to supporting exactly this profile, always, with no compile-time flags.
3. **The semantic join spans the boundary.** `sources[].id` (frontmatter) is joined by footnote *labels* in the body (OKF §5.1, "per-claim attribution"). A grammar that stops at the delimiter makes the single most OKF-specific relation in the format unrepresentable in one query.

### 1.2 Non-goals

| Non-goal | Why |
|---|---|
| Validating OKF conformance | The grammar is permissive by construction (OKF §11 is deliberately tolerant). Conformance is a separate, host-side pass. |
| Enforcing that `type` is present | OKF §4.1 makes it required *conceptually*, but OKF §11-style tolerance means the grammar must parse a concept with no frontmatter at all. Required-field checks are a query/lint concern. |
| Interpreting YAML semantics | Scalar resolution (is `2026-12-31T00:00:00Z` a timestamp? is `0.2` a string or a number?) is host-side. See D3. |
| Deriving trust tiers, staleness, or graph closure | Requires absence tests (OKF §5.3: "No `verified` key ⇒ unverified") and cross-file lookup. Impossible in a `queries/*.scm` match. See D5 and §7.5. |
| Resolving or checking links | Broken cross-links are explicitly conformant (OKF §6.1, OKF §11). |
| I/O, bundle discovery, directory walking | Not a parser's job. |
| Formatting / round-tripping | The grammar is lossless (every byte is in a named node or `extras`), but a canonicalizer is a separate tool. |
| The `[[WikiLink]]` / `#hashtag` dialect | Not in OKF v0.2. See D7. |

### 1.3 Ecosystem context

Measured 2026-09-22 against npm and GitHub while drafting:

* **No tree-sitter grammar for OKF exists** — `tree-sitter-okf` is unclaimed on npm (`404`). This is greenfield.
* **Nearest neighbours, and what they do and don't cover:**
  * `tree-sitter-markdown` 0.7.1 (split into a *block* grammar and a *markdown-inline* grammar, sharing `common/common.js`). Supports a `minus_metadata` block for `---` frontmatter as a **compile-time env-var flag** (`EXTENSION_MINUS_METADATA`), plus GFM flags for pipe tables, task lists and strikethrough. **No footnotes** — upstream has no footnote rule at all, and OKF v0.2 relies on them.
  * `tree-sitter-yaml` 0.5.0 — general-purpose YAML.
  * `mrqc/bitomb` (Rust/Pest) — an OKF-*like* dialect that adds `[[WikiLink]]` and `#tag` tokens not present in OKF v0.2.
  * `franklinbaldo/okf-parser` (Python/Ibis) — bundle-level relational inspection; the closest prior art for *what consumers want to query*, and a useful source of query requirements.
  * The wider ecosystem (per `openknowledgeformat.com/ecosystem`) is CLI/linter/MCP-shaped — Go, Rust, Ruby, Python. **None of them ship a tree-sitter grammar**, so this would be the first editor- and LSP-grade artifact in the ecosystem.

The gap this fills: every existing tool re-implements frontmatter scanning by hand. One grammar + one query library replaces all of that, and gives editors highlighting, folding, structure outlines, and injections for free.

### 1.4 Prior art we should deliberately borrow from

| Source | Borrow | Don't borrow |
|---|---|---|
| `tree-sitter-markdown` | Node names, field names, external scanner design for CommonMark block/inline disambiguation | Its compile-time extension flags (see D4) |
| `tree-sitter-yaml` | Node names for mappings/sequences/scalars, so the opt-in injection preset (D2) is query-compatible | Its error behaviour and its full-grammar weight |
| `okf-parser` | The query surface: identity, lineage, cardinality, provenance as relations | Its bundle-level scope |

---

## 2. Design principles and decisions

### 2.1 Principles

| | Principle | Consequence |
|---|---|---|
| **P1** | **Lossless and structure-first.** | Every byte belongs to a node or an `extras`-allowed token. No re-serialization guessing. |
| **P2** | **The grammar is filename-agnostic.** | No `index_document` / `log_document` root. Filename-dependent meaning is host-side (§4.4). |
| **P3** | **Syntactic nodes only.** | A node exists iff its presence is decidable by looking at *this file's syntax*. Everything else is a query or host code (§7.5). |
| **P4** | **Plausible input never produces `ERROR`.** | Malformed frontmatter degrades to an opaque node, never poisons the body (§8). |
| **P5** | **Self-contained by default.** | Core OKF queries work with only this grammar loaded. Injections are an enhancement, not a dependency (§7.6). |
| **P6** | **Vocabulary mirrors upstream.** | `concept`, `frontmatter`, `body`, `link`, `source` — the nouns in upstream SPEC §2. |
| **P7** | **Node names are semver'd API.** | Renaming a node is a breaking release. Add a deprecation table when it happens (§10.3). |

### 2.2 Decisions

#### D1 — Scope: parse the whole document in one grammar

| Option | Assessment |
|---|---|
| (a) Frontmatter-only grammar; delegate body to `tree-sitter-markdown` | Fails the §1.1(3) requirement — `sources[].id` ↔ footnote-label joins become impossible in a single query. |
| (b) Markdown-only, frontmatter as `minus_metadata` blob | Same failure; also loses the `# Computation` / `# Schema` structural conventions. |
| **(c) One grammar, whole document** | **Recommended.** |

**Cost of getting it wrong:** if we pick (a) or (b), downstream tools re-implement frontmatter scanning — which is exactly the status quo we are trying to replace.

#### D2 — Frontmatter strategy

| Option | Assessment |
|---|---|
| (a) Opaque `frontmatter` node + `injections.scm` → `tree-sitter-yaml` | Cheapest to build; **breaks P5** — `tree-sitter parse` output has no field nodes, so the OKF query library silently returns nothing unless the consumer also installs and wires up a second grammar. |
| (b) Vendor `tree-sitter-yaml`'s grammar into ours | Full YAML fidelity, but drags in a heavyweight external scanner whose states interact with ours, and inherits `ERROR` behaviour on valid-for-OKF-but-odd-for-YAML inputs (see Appendix A). |
| **(c) Purpose-built "OKF-YAML" subset, native, with a never-fail fallback** — plus (a) available as an *opt-in* injection preset | **Recommended.** |

**Rationale.** The frontmatter corpus is narrow and measurable: 16 distinct keys, five scalar forms, two collection forms, zero anchors, zero aliases, zero block scalars, zero `#` comments across all 54 frontmatter blocks in the official bundles (Appendix A). Against that, OKF §11.6 demands we "MUST NOT reject documents with unrecognized fields", so the dominant risk is not *missing* YAML features — it is **failing on an input we did not anticipate**. A ~400-line purpose-built subset with an explicit, named escape hatch (`yaml_unsupported`, §5.1) is easier to keep total than a general YAML parser.

**Naming.** The layer MUST be called **OKF-YAML**, never "YAML", in docs, node docs and error messages. The out-of-subset list (§5.1) is normative and maintained; anything on it produces `yaml_unsupported`, not a wrong parse.

**Mitigation for the fidelity risk:** the scalar *values* extracted by our layer are differentially tested against a reference YAML parser over the corpus and a YAML-suite subset (§9.4).

**Opt-in preset:** ship `queries/injections-fullyaml.scm`, which delegates the frontmatter content to `tree-sitter-yaml` for hosts that need full YAML. Off by default.

#### D3 — Should the grammar resolve scalar *types*?

| Option | Assessment |
|---|---|
| (a) Emit `integer_scalar`, `boolean_scalar`, `null_scalar`, `timestamp_scalar`, … | Requires us to pick YAML 1.1 vs 1.2 resolution rules (`yes` vs `true`, `0.2` vs `"0.2"`). OKF §12 uses a *quoted* `okf_version: "0.2"` precisely to dodge this. Wrong resolution = silent data corruption. |
| **(b) Emit `plain_scalar` / `single_quote_scalar` / `double_quote_scalar` / `block_scalar` only; expose typing as `#match?` patterns in `queries/okf/scalars.scm`** | **Recommended.** The grammar stays faithful to the bytes; the typing policy is a replaceable, testable, per-consumer artifact. |

Divergence from `tree-sitter-yaml` here is deliberate and MUST be recorded in the §7.6 alignment table so the injected preset isn't silently inconsistent.

#### D4 — Markdown fidelity strategy

This is the largest cost item. Three strategies:

| Option | Build cost | Fidelity risk | Ongoing cost |
|---|---|---|---|
| (a) Opaque `body` + inject `tree-sitter-markdown` | ~0 | n/a | **Breaks P5**, and `tree-sitter-markdown`'s GFM is compile-time-gated — the npm build may not have tables, so OKF's `# Schema` tables degrade unpredictably. Unacceptable. |
| (b) **Merge strategy** — vendor `tree-sitter-markdown`'s block + inline grammars into one grammar, pin the profile ON, add footnotes and the OKF frontmatter layer | High (≈2 000 lines vendored + scanners) | Low — inherits a battle-tested CommonMark implementation | Re-vendor on upstream releases (`MERGE.md` + `script/revendor`) |
| (c) **From-scratch profile subset** — write the ~700-line "OKF markdown profile" ourselves | Medium | **High** — CommonMark has 652 spec examples; every miss is a mis-parse that surfaces in someone's language server | We own the treadmill |

**Recommendation: (b), the merge strategy.**

**Rationale.** A grammar's entire value proposition here *is* fidelity, and the OKF delta over "CommonMark + GFM tables + GFM footnotes" is small: a frontmatter layer, a footnote definition rule, the profile pinned on, and the OKF query library. Vendoring lets us spend the budget on the delta and on tests instead of on re-deriving CommonMark rules that already work. It also preserves node names that existing markdown queries and injections in the wild already target (D10, §7.5).

**Profile pinned ON (no compile-time flags — this is the point of having our own grammar):**

* `EXTENSION_MINUS_METADATA` → replaced by our own frontmatter rules (D2)
* `EXTENSION_PIPE_TABLE` (GFM) — **required** for `# Schema` (OKF §4.2)
* `EXTENSION_GFM` strikethrough, task list
* **Footnote definitions and references** — new; upstream `tree-sitter-markdown` has no footnote rule (OKF §5.1 requires them)
* `EXTENSION_TAGS`, `EXTENSION_WIKI_LINK`, `EXTENSION_PLUS_METADATA`, `EXTENSION_LATEX` → **OFF** (D7). Keep the rules in the vendored tree behind our own flag so a dialect profile is a build option, not a fork.

**Repo-layout requirement:** vendored code lives in `vendor/tree-sitter-markdown/` with the upstream commit pinned in `vendor/LOCK.json`, merged by `script/revendor` (a three-way merge, not `cp -r`), and `MERGE.md` documents every local delta. `make verify-vendor` fails CI if the delta list and the tree disagree.

**Escape hatch:** if the merge proves unmaintainable by M3, fall back to (c) *scoped to the profile* — the node names and queries are unchanged either way. This is a lossless pivot, which is why it is worth naming now.

#### D5 — Grammar vs. queries boundary

**Rule (P3):** a node exists **iff** its presence is decidable from this file's syntax alone.

| Belongs in the grammar | Belongs in queries | Belongs in host code |
|---|---|---|
| frontmatter delimiter, mapping, sequence, scalars, comments | "this pair is the `type` field" | "is `type` non-empty?" (OKF §11.2) |
| ATX/setext heading, fence, list, table, block quote | "this heading is `# Computation`" (OKF §4.2) | — |
| link text, destination, title; footnote ref; footnote def | "this destination is bundle-absolute / relative / external" (OKF §6.2) | "does the target exist?" (OKF §6.1) |
| inline code, emphasis, escapes | "this inline code is a runtime name" | — |
| — | "find all frontmatter `sources[].id`" | "do all footnote labels resolve to a `sources[].id`?" |
| — | — | trust tier (OKF §5.3), staleness (OKF §5.5), conformance (OKF §11) |

**Consulted but not adopted:** *"privilege the OKF keys (`type`, `sources`, …) as grammar fields."* Attractive for highlighting, but OKF §4.1 explicitly permits arbitrary extra keys and OKF §11.6 forbids rejecting unknown ones, and the key set grows every minor version (OKF §12). A hard-coded key list in the grammar would be a compatibility liability. Instead, `queries/okf/fields.scm` defines a named capture per well-known key, versioned with the query library rather than with the parser.

#### D6 — Filename awareness

**Recommendation: none.** The grammar sees bytes, not paths; tree-sitter provides no filename channel. Consequences to accept and document:

* A file with no frontmatter is a valid tree. It might be `index.md` (OKF §8), a `log.md` without frontmatter, or a non-conformant concept. Classification is `classify(path, tree)`, specified in §4.4 and shipped as a documented helper (README + a reference implementation per binding), **not** as a node.
* `okf_version` (OKF §12) is meaningful only in a bundle-root `index.md`. The grammar parses the pair like any other; §7.3 defines the derived capture.

#### D7 — Dialects: `[[WikiLink]]`, `#tag`, `+metadata`

**Recommendation: OFF by default, build-optional.** OKF v0.2 defines cross-links as *standard markdown links* only (OKF §6.1) and tags as a *frontmatter list* (OKF §4.1; OKF §3.1 explicitly declines a tag-file format). `mrqc/bitomb` supports `[[WikiLink]]`/`#tag`, and `tree-sitter-markdown` has a disabled `EXTENSION_WIKI_LINK` — so the dial *exists* upstream, but turning it on would make `[[Foo]]` a `wiki_link` node in every OKF document and quietly change what "cross-link" means. Keep the vendored rules, gate them behind `--features dialect-wikilink` at build time, and document the dialect as experimental. **Q7.**

#### D8 — Error-recovery posture

| Option | Assessment |
|---|---|
| Strict — malformed input yields `ERROR` | Fails P4. Every lint tool then reports "parse error" for a frontmatter typo and loses the body's links. |
| **Graceful — degrade to opaque nodes, reserve `ERROR` for genuinely unsalvageable bytes** | **Recommended.** |

Two mechanisms:

1. **`MISSING` token insertion** where a delimiter is required but absent (e.g. unterminated frontmatter, OKF §4.2). This preserves the *parse* and gives the host an exact, actionable diagnosis.
2. **`yaml_unsupported` / `html_block`-style opaque leaves** where content is outside the subset — damage is *localised to the smallest enclosing unit* (a bad pair consumes to the next dedent, never the rest of the file).

**Acceptance test:** the zero-`ERROR` property over the whole pinned corpus + the fuzz corpus (§9.3).

#### D9 — Setext headings

Not present in the corpus (Appendix A), but valid CommonMark and cheap once vendored. **Recommendation: support**, using CommonMark's one-line-lookahead rule. Two interactions to test explicitly:

* `---` following a paragraph line is a setext H2, **not** a thematic break.
* Frontmatter is recognised only at byte offset 0 (BOM-tolerant), so a later `---` can never be re-interpreted as a frontmatter delimiter — the ambiguity OKF §4 leaves open is closed by the grammar. **Q4.**

#### D10 — Node-name alignment with upstream grammars

**Recommendation: reuse `tree-sitter-markdown` and `tree-sitter-yaml` names wherever a construct corresponds; prefix or rename only where we genuinely diverge, and record every divergence in §7.6.** Payoff: existing `highlights.scm` / `injections.scm` written for markdown mostly work on OKF bodies, and the D2 injected preset is near-query-compatible. Cost: we inherit upstream naming warts. Worth it.

#### D11 — Language-feature registration (`.md` collision)

`.md` is already claimed by `tree-sitter-markdown` in every major editor. **Recommendation:** because our profile is essentially CommonMark + GFM tables + footnotes + frontmatter, registering `okf` for `markdown` is *safe* — it is a superset. Ship:

* README recipes for VS Code (`files.associations`) and Neovim (`vim.treesitter.language.register('okf', 'markdown')`), scoped per project where possible;
* `tree-sitter.json` file-type metadata;
* **Q12**: if the target is a bundle-wide LSP rather than an editor, bulk registration is a non-issue and this can be deprioritised.

#### D12 — Versioning and ABI

**Recommendation:** develop against CLI ≥ 0.25 (0.27.0 current); **generate and ship ABI 14 as the floor** (broad editor compatibility) while verifying ABI 15 works, and record both in `tree-sitter.json`. Node-type changes follow semver (P7); the query library is versioned with the `okf.spec_version` it targets (`0.2`), not with the parser's semver. **Q14** asks whether to dual-publish generated parsers per ABI.

---

## 3. Input model

### 3.1 Byte-level rules

| Aspect | Rule |
|---|---|
| Encoding | UTF-8. A leading `U+FEFF` BOM MUST be accepted and MUST NOT prevent frontmatter recognition. |
| Line endings | `LF` and `CRLF` MUST both parse. `\r` is part of the line terminator, never part of a scalar. (Corpus: all LF, but `okf-parser` lists BOM/CRLF handling as a hard-won lesson — treat as required.) |
| Final newline | Optional. |
| Tabs | Permitted; in frontmatter they are YAML indentation, in markdown they are code indentation. Both must be handled by the scanners (§5.2, D4). |
| NUL / invalid UTF-8 | Host's problem; the grammar treats bytes transparently. |
| Empty file | `source_file` with an empty `body`. Never `ERROR`. |
| Size | Target: linear time. OKF files are small (corpus max ≈ 20 KB) but bundles are not (§9.5). |

### 3.2 Document shapes the grammar recognises

```ebnf
source_file        = [ frontmatter ] , body ;
frontmatter        = opening-delimiter , [ okf-yaml-content ] , [ "..." ] , closing-delimiter ;
opening-delimiter  = [ BOM ] , "---" , line-ending ;   (* byte offset 0 only *)
closing-delimiter  = "---" , ( line-ending | eof ) ;   (* column 0, not inside a scalar *)
body               = block* ;
```

That is the whole top level. There is no `concept_document` / `index_document` / `log_document` node (D6); the three differ only in how a host classifies them (§4.4).

---

## 4. Document-level grammar

### 4.1 `source_file`

* Field `frontmatter` (optional) — §5.
* Field `body` (mandatory, possibly empty) — §6.

Both fields are public so `(source_file frontmatter: … body: …)` reads naturally in queries.

### 4.2 Frontmatter delimiters and recovery

| Input | Expected tree |
|---|---|
| `---⏎…⏎---⏎body` | `frontmatter`, then `body`. |
| `---⏎…⏎---⏎` (empty body) | `frontmatter`, empty `body`. |
| `---⏎---⏎body` (empty frontmatter) | `frontmatter` with no content node. **Valid** — a host-side conformance check flags the missing `type`. |
| `---⏎type: X⏎body` (no closing `---`) | `frontmatter` with a **`MISSING "---"`** at EOF; content parsed best-effort; empty `body`. The diagnosis is "missing closing delimiter", not "parse error". |
| `body` with no opener | No `frontmatter`; whole file is `body`. **Not an error** (D6). |
| `----` / `--- ` (trailing space, 4+ dashes) at offset 0 | Not a delimiter → whole file is `body`. |
| BOM then `---` | Delimiter recognised (BOM is an `extras` token). |

Additional constraints to encode:

* The closing delimiter is recognised **only at column 0** and **only when the scanner is not inside** a block scalar or a multi-line plain scalar continuation. A `---` inside `description: foo⏎  ---` does not close the block. This is the single most dangerous ambiguity in the frontmatter layer. **Q5.**
* `...` (YAML document-end marker) MAY appear immediately before the closing `---`; it is an opaque `yaml_document_end` leaf.
* `---` at column 0 inside a fenced code block in the *body* is fence content (handled by the body layer, no interaction).

### 4.3 Body

`body` = `repeat(block)` over the profile in §6. A `body` with no blocks is empty and valid.

### 4.4 Classification (host-side helper, normative for the README)

Not a node (D6). Specified so all bindings implement the same thing:

```
classify(path, tree):
  basename = "index.md"   -> :index    (frontmatter permitted only at bundle root, OKF §8 / OKF §12)
  basename = "log.md"     -> :log      (the official log.md carries frontmatter `type: Log`)
  tree has no frontmatter  -> :index, :log, or :non_conformant   # ambiguous; report as such
  otherwise                -> :concept
```

The middle case is exactly why this is a documented *helper* and not a grammar decision: OKF §11.3 keys conformance off the filename, and `index.md` vs. `log.md` is not decidable from bytes.

---

## 5. Frontmatter grammar (OKF-YAML)

### 5.1 The subset — normative

**In subset (MUST parse into the nodes in §7.2):**

1. Block mappings: `key: value`, nested by indentation.
2. Block sequences: `- item`, **including the zero-indent form** where the dash is at the *same* column as its parent key (Appendix A — the official bundles do this for `tags:`, `sources:`, and `verified:`).
3. Block sequences of block mappings: `- term: "…"` / `  why: "…"` (Appendix A `metrics/gross-margin.md`, the `not:` key).
4. Flow mappings: `{ by: human:jsmith@acme, at: 2026-07-01T09:00:00Z }` — 21 occurrences in the corpus, including nested in sequence items (`- { name: year, type: integer, required: true }`).
5. Flow sequences: `[job_id, executed_sql, result]`, `[sales, orders, revenue]`.
6. Plain scalars — single-line **and multi-line folded** (`description: … containing⏎  user interaction logs.`; Appendix A, `ga4/tables/events_.md`). Plain scalars MUST tolerate `&`, `*`, `#`, `:` and `[]` *inside* the value (real corpus values contain `&` and `:` and unbracketed `*`), while still treating a leading `&`/`*`/`!` as an anchor/alias/tag.
7. Single-quoted scalars with `''` escaping; double-quoted scalars with backslash escapes. Quoted scalars MAY contain `#`, `:`, `[`, `]`, `{`, `}`, `,` (corpus: `'Google Analytics Help: BigQuery Export Schema'`, `"revenue minus full COGS (product cost + …)"`).
8. Block scalars `|`, `>`, with chomping (`-`, `+`) and explicit-indentation indicators.
9. Comments: `# …` to end of line, in every position YAML allows (the OKF §5.5 example uses a trailing comment after `stale_after: …`). Comment text is a `comment` node.
10. Anchors (`&name`), aliases (`*name`), tags (`!`/`!!`), and explicit keys (`? … : …`) — recognised as **opaque leaves**, never interpreted (D3).
11. Multi-line flow collections (a `{`/`[` that does not close on the same line).
12. Empty values (`key:` with nothing after) and empty documents.
13. A leading `%YAML`/`%TAG` directive line — opaque `yaml_directive`.

**Out of subset (MUST produce `yaml_unsupported`, MUST NOT produce `ERROR`):** structured values under complex/`?` keys; multi-line quoted scalars with escaped line breaks; merge keys (`<<`) — recognised, not applied; and anything else YAML 1.2 allows that this list does not. The list is maintained in `docs/okf-yaml.md`; **Q3** asks whether the split is drawn in the right place, and **Q11** asks whether `yaml_unsupported` is the right public name.

### 5.2 Indentation model

An external scanner owns indentation (the one genuinely novel scanner in the project):

* Tokens: `_indent` (increase), `_dedent` (decrease), `_same_indent` (sibling).
* Column-0 block sequences after a bare key (Appendix A) are an explicit case, not an accident: after `key:` with an empty value, a `-` at the *parent key's* column opens a sequence owned by that key (YAML's "a block sequence may be at the same indentation as its parent mapping key" rule).
* Tabs count as indentation with a documented, fixed tab width (YAML forbids tab *indentation*; we must still not crash — **Q6**).
* The scanner also owns `_frontmatter_open` / `_frontmatter_close` (§4.2) so the delimiter test and the scalar-continuation state live in one place.

**Risk:** two scanners' states (markdown block/inline, from the vendored tree, and OKF-YAML) coexist in one C file (`src/scanner.c`). The frontmatter region and the body region are disjoint, so state does not need to be shared — but the scanner MUST refuse to advance in the wrong region. This is the **highest implementation risk in the project**; §9.6 is the mitigation.

### 5.3 Pair `key` / `value` fields

Every mapping pair exposes fields `key` and `value` (matching `tree-sitter-yaml`). `block_sequence_item` wraps its value directly, so `tags:` yields `(block_sequence (block_sequence_item (plain_scalar)))` — flat and query-friendly, with no artificial `value:` wrapper on items.

### 5.4 Duplicate keys and unknown constructs

YAML forbids duplicate keys; OKF has no rule about them. **Recommendation: parse them, do not error**, and let a host-side linter flag duplicates — same posture as D8: the grammar reports structure, not verdicts.

---

## 6. Body grammar (OKF markdown profile)

### 6.1 Block constructs (normative)

| Construct | Node | OKF relevance |
|---|---|---|
| ATX heading | `atx_heading` + `atx_h1_marker`…`atx_h6_marker` + `heading_content` | `# Schema`, `# Examples`, `# Computation` (OKF §4.2); `## YYYY-MM-DD` in `log.md` (OKF §9) |
| Setext heading | `setext_heading` + `setext_h1_underline` / `setext_h2_underline` | CommonMark parity (D9) |
| Paragraph | `paragraph` → `inline` | — |
| Block quote | `block_quote` | — |
| Bullet / ordered list | `list`, `list_item`, `list_marker_star` / `_minus` / `_plus` / `_dot` / `_parenthesis` | `index.md` entries (OKF §8); `log.md` entries (OKF §9) |
| Fenced code block | `fenced_code_block`, `info_string`, `language`, `code_fence_content` | `# Computation` payload (OKF §10.3); `# Examples` (OKF §4.2) |
| Indented code block | `indented_code_block` | **Required** — upstream §10.2/§10.3 put the inline computation in a 4-space block. Zero occurrences in the sample bundles, so fixtures are hand-written (Appendix A) |
| Thematic break | `thematic_break` | — |
| Pipe table (GFM) | `pipe_table`, `pipe_table_header`, `pipe_table_delimiter_row`, `pipe_table_delimiter_cell`, `pipe_table_row`, `pipe_table_cell` | `# Schema` (OKF §4.2, OKF §4.3) |
| Link reference definition | `link_reference_definition` | — |
| **Footnote definition (GFM)** | `footnote_definition`, `footnote_label` | OKF §5.1: "Per-claim attribution … a markdown footnote whose label is a `sources[].id`" |
| HTML block | `html_block` (opaque) | Permitted by CommonMark; not used in the corpus |
| Blank line | `extras` / hidden | — |

### 6.2 Inline constructs (normative)

| Construct | Node | OKF relevance |
|---|---|---|
| Text / soft line break | `text`, `soft_line_break` | — |
| Emphasis / strong / strikethrough | `emphasis`, `strong_emphasis`, `strikethrough` | Table-cell emphasis is in the corpus (`*event_params.key*`) |
| Code span | `code_span` | Pervasive; **nests inside link text** in the corpus: ``[`computations/revenue-ytd.md`](./revenue-ytd.md)`` |
| Inline link | `inline_link` → `link_text`, `link_destination`, `link_title` | OKF §6.1 — cross-links |
| Full / collapsed / shortcut reference link | `full_reference_link`, `collapsed_reference_link`, `shortcut_link` | — |
| Image | `image` → `image_description` / `link_destination` / `link_title` | Not in corpus; CommonMark |
| Autolinks | `uri_autolink`, `email_autolink` | — |
| **Footnote reference (GFM)** | `footnote_reference`, `footnote_label` | OKF §5.1 — 45 references and 34 definitions in the corpus |
| Hard line break | `hard_line_break` | Not in the corpus; CommonMark |
| Backslash escape | `backslash_escape` | — |
| Entity / numeric character reference | `entity_reference`, `numeric_character_reference` | — |
| Inline HTML | `html_tag`, `html_comment` (opaque) | — |

### 6.3 Conventional headings are **not** privileged nodes

`# Computation`, `# Schema`, `# Examples` (OKF §4.2) get no grammar node. They are defined by `queries/okf/sections.scm` as captures on `heading_content` matched case-insensitively. **Rationale:** the set grows with every minor version of the spec (OKF §12) and, per OKF §4.2, the list is a SHOULD, not a MUST. Hard-coding it in the grammar would repeat the D5 mistake at the body layer.

The *pairing* rule — "the computation is the single fenced or indented code block that follows `# Computation` before the next heading of the same or higher level" — is likewise a query (§7.3).

### 6.4 Deliberately not in the profile

LaTeX blocks, wiki links, `#`-tags, `+metadata`, and task-list checkbox *semantics* (the GFM `- [ ]` marker is parsed for CommonMark fidelity but gains no OKF meaning). See D7.

---

## 7. Node reference, queries, and the derived model

### 7.1 Tree shape at a glance

```
(source_file
  (frontmatter
    (block_mapping
      (block_mapping_pair key: (plain_scalar) value: (plain_scalar))
      (block_mapping_pair key: (plain_scalar) value: (flow_mapping
        (flow_pair key: (plain_scalar) value: (plain_scalar)) …))
      (block_mapping_pair key: (plain_scalar) value: (block_sequence
        (block_sequence_item (flow_mapping …))))))
  (body
    (section
      (atx_heading (atx_h1_marker) (heading_content))
      (paragraph (inline (text)))
      (fenced_code_block (info_string (language)) (code_fence_content)))))
```

(`section` groups a heading with its content, as in `tree-sitter-markdown`. **Q8**: whether to keep the `section` wrapper, or flatten `body` to a list of sibling blocks. Keeping it matches upstream; flattening makes "the fence under `# Computation`" a sibling match instead of a tree walk.)

### 7.2 Public node inventory

Full list with names, fields, and upstream alignment: `docs/node-types.md`, generated from `node-types.json` and diffed in CI (`make check-nodes`) so §7.6 cannot drift. Q8's answer changes this file, so generate it *after* Q8 is settled.

### 7.3 Query library (`queries/`)

| File | Purpose |
|---|---|
| `highlights.scm` | Highlighting |
| `injections.scm` | `fenced_code_block` → `injection.language` from `info_string`; `html_block` → `html` |
| `injections-fullyaml.scm` | Opt-in: `frontmatter` content → `yaml` (D2) |
| `locals.scm` | Heading scope (`@local.scope` on headings, `@local.definition` on their content) — enables structure outlines and region folding |
| `tags.scm` | `@definition.class` on concept documents, `@reference.class` on cross-links |
| `okf/fields.scm` | One named capture per well-known frontmatter key: `type`, `title`, `description`, `resource`, `tags`, `generated`, `verified`, `status`, `stale_after`, `sources`, `usage_window`, `runtime`, `parameters`, `computation`, `executor`, `attester`, `okf_version` |
| `okf/links.scm` | Classifies `link_destination` by form — see §7.4 |
| `okf/sources.scm` | `sources[].id`, `sources[].resource`, `sources[].author`, `usage_count`, `last_modified` |
| `okf/scalars.scm` | Typing policy (D3): integer / timestamp / boolean-ish patterns |
| `okf/sections.scm` | Conventional headings (§6.3) and `log.md` date headings (`## YYYY-MM-DD`) |
| `okf/citations.scm` | `footnote_definition` / `footnote_reference` labels, for the `sources[].id` join |
| `okf/computation.scm` | The `# Computation` → code-block pairing (§6.3) |

Every file in the list above is **node-matching plus `#match?` only** — no `#eq?` against sibling order, no host callbacks — so the same queries run in every host.

### 7.4 Link classification (the `#match?` contract)

`queries/okf/links.scm` is normative for "what kind of path is this?" (OKF §6.2). Captures:

| Capture | Pattern on the destination text |
|---|---|
| `okf.link.uri` | `^\w+:` (contains a scheme — `https:`, `mailto:`, …) |
| `okf.link.bundle_relative` | `^/` (OKF §6.1, "recommended form") |
| `okf.link.relative` | `^\.{0,2}/` or a bare pathspec not matching the above |
| `okf.link.fragment` | `^#` |

Consumers MUST treat these as *classifications of a path-shaped string*, not as resolution: `sources[].resource` may be a **scope descriptor** rather than a path (OKF §5.1, e.g. `all queries in BigQuery project X`), so a capture firing means "path-shaped", never "resolvable".

### 7.5 What cannot be a query (and therefore must be host code)

Specified here so every binding behaves identically; implemented per binding:

| Derived fact | Why it can't be a query |
|---|---|
| Trust tier (unverified / machine-confirmed / human-reviewed, OKF §5.3) | Needs *absence* of `verified` and a prefix test over a list. |
| `verified` written as a bare mapping treated as a one-element list (OKF §5.2) | Shape coercion across two node shapes — host-side normalisation. |
| Staleness (`now >= stale_after`, OKF §5.5) | Needs a clock. |
| Conformance (OKF §11) | Needs the filename, the frontmatter's non-emptiness, and multi-file context. |
| Link resolution, orphan detection | Cross-file. |
| Legacy v0.1 fallbacks (`timestamp` → `generated.at`, `# Citations` → `sources`, OKF §13.1) | Absence + precedence. |
| `concept_id` (path minus `.md`) | Needs the path. |
| Duplicate frontmatter keys (§5.4) | Requires counting within a mapping. |

### 7.6 Node-name alignment table

Lives in `docs/node-types.md`, one row per public node: `node`, `fields`, `upstream source` (`tree-sitter-markdown` / `tree-sitter-yaml` / `okf-new`), and `divergence` (empty, or the reason). CI fails if a node in `node-types.json` has no row. Payoff and cost in D10.

---

## 8. Error handling policy

| Situation | Treatment | Rationale |
|---|---|---|
| Unterminated frontmatter | `MISSING "---"` at EOF | Actionable: "add a closing delimiter" |
| `yaml_unsupported` content | Opaque leaf to the next dedent | Damage localised; the body still parses |
| Unclosed fence | Fence content to EOF | CommonMark behaviour |
| Malformed table (no delimiter row) | Falls back to `paragraph` | CommonMark behaviour |
| `[^label]` with no matching definition | Parses as `footnote_reference` | Broken references are conformant; host lint |
| Missing `type` | Parses fine | OKF §4.1, OKF §11 — permissiveness is the design |
| Unknown key | `block_mapping_pair` like any other | OKF §11.6 |
| `ERROR` nodes | Reserved for genuinely unsalvageable bytes — target **zero** on the corpus | P4 |

The distinction that matters: `ERROR` means *the grammar could not proceed*. Everything else is a host-visible *finding*. `tree_sitter_okf` must never make a host guess which of those it is looking at.

---

## 9. Testing and conformance

### 9.1 Layout

```
test/
  corpus/            hand-written .txt trees (tree-sitter test format)
    frontmatter/     delimiters, recovery, scalars, collections, indentation
    body/            one file per block and inline construct
    documents/       whole-document golden trees
    errors/          every row of §8 (asserts exact ERROR/MISSING placement)
  fixtures/bundles/  pinned copies of the four official bundles
  queries/           query result fixtures, incl. the dual-mode ones (§9.4)
```

### 9.2 Fixture provenance

The official bundles are Apache-2.0 (verified) → vendorable with attribution and a NOTICE file. `test/fixtures/bundles/LOCK.json` pins the upstream commit (`ad30107…`) plus a per-file SHA-256; `script/sync-fixtures` refreshes, and CI fails if the lock and the files disagree. This is the difference between "we test against a snapshot" and "we test against whatever `main` is today".

### 9.3 Property tests

| Property | Assertion |
|---|---|
| **Zero `ERROR`** | Over corpus + fixtures + a fuzz corpus of mutated inputs, no `ERROR` node exists |
| **Lossless** | Concatenating leaf byte ranges in order reconstructs the input exactly |
| **Coverage** | Every byte is inside some named node or `extras` |
| **Determinism** | No time-, locale-, or hash-order-dependent behaviour |
| **Scanner state isolation** | Re-parsing an unchanged tree yields identical trees (guards against leaks between the two scanners) |
| **Incremental == full** | For N random edits: incremental tree ≡ fresh tree. This is where scanners usually break, and corpus tests never see it |
| **Linear time** | Timing ratio for 2× input stays under a documented bound |

### 9.4 Differential tests

1. **Frontmatter values vs. a reference YAML parser.** Extract every scalar via the tree, resolve it with a host YAML library, and compare against the tree's byte span (D2's mitigation). Normalises away quoting style; catches folded-plain-scalar and block-scalar errors — the failure modes corpus tests miss.
2. **Body vs. `tree-sitter-markdown` with GFM enabled.** For each fixture, the subsequence of block/inline nodes inside `body` matches upstream's node sequence for the same text. Any diff is either a recorded intentional divergence or a bug.
3. **Query portability (§7.6).** The same shared query fixtures run against native and injected frontmatter modes.

### 9.5 Performance

Bench: parse all bundles, report files/sec and p99 for a single 100 KB synthetic document; trend against a committed baseline (`bench/`). Gates: no quadratic blowup, linear memory, incremental re-parse of a 1-line edit < 1 ms on the corpus. **Q13**: is an editor-latency budget or a bulk-index budget the priority? It changes how hard we tune the scanners.

### 9.6 Risk register

| Risk | Severity | Mitigation |
|---|---|---|
| Two scanners (markdown + OKF-YAML) in one `scanner.c` diverge on state | **High** | Region-disjoint by construction; the incremental-equals-full property test (§9.3); the frontmatter scanner never runs past the closing delimiter |
| Vendored markdown becomes unmaintainable | Medium | `MERGE.md` delta list + `script/revendor` + `make verify-vendor`; the D4(c) escape hatch is lossless |
| OKF-YAML accepts something YAML doesn't, or vice versa | Medium | Differential value test (§9.4); explicit out-of-subset list; `yaml_unsupported` is grep-able in CI |
| A future OKF minor version adds a construct we called out-of-subset | Low | Out-of-subset list is versioned with `okf.spec_version`; adding a rule is non-breaking |
| Corpus-driven over-fitting | Low | Fuzz corpus + CommonMark spec examples inside the profile |

---

## 10. Packaging and delivery

### 10.1 Repo layout

```
tree-sitter-okf/
  grammar.js                 # our rules + the merged markdown rules
  src/scanner.c              # markdown scanner (vendored) + OKF-YAML scanner (new)
  vendor/tree-sitter-markdown/{LOCK.json,MERGE.md}   # provenance of merged rules
  queries/                   # §7.3
  docs/grammar-spec.md       # this document
  docs/node-types.md         # generated, CI-diffed (§7.6)
  docs/okf-yaml.md           # normative in-subset / out-of-subset list (§5.1)
  test/                      # §9.1
  bench/  script/  bindings/  tree-sitter.json  package.json  Cargo.toml  README.md
```

### 10.2 Bindings

Node (primary, `tree-sitter-okf` on npm — verified unclaimed), then Rust (`tree-sitter-okf` crate), Python, C, Go, Swift. Bindings are generated by `tree-sitter generate`; hand-written host helpers (the §4.4 classifier, the §7.5 derived facts) ship per binding with identical semantics and a shared conformance fixture file.

### 10.3 Versioning and release

* Parser semver is decoupled from OKF's `0.x` (P7). Releases carry `parser 1.0.0`, `okf.spec_version 0.2`, `query library 0.2.x`.
* CI: `generate` reproducibility (two runs byte-identical), `test`, `check-nodes`, `verify-vendor`, `parse --quiet --stat` over fixtures, query fixtures, property suite, bench.
* `main` is always green and ABI-compatible; editor-facing changes are announced in `CHANGELOG.md` with a node-rename table.

---

## 11. Open questions for review

| # | Question | Why it matters | My lean |
|---|---|---|---|
| **Q1** | Is the unit **one document** (as specified) or should the grammar model a **bundle** (a whole tree)? | Tree-sitter grammars parse one buffer. If the primary consumer needs bundle-level relations, that layer is a separate tool built on this tree, and we should say so in §1.1. | One document; bundle tooling composes on top |
| **Q2** | Is the primary consumer an **editor/LSP** (highlighting, folding, injections) or a **tool/CI** (linting, indexing, extraction)? | Changes query-library emphasis and the D11 registration story. | Editor + tool, editor first |
| **Q3** | Is the in-subset / out-of-subset line in §5.1 drawn correctly — specifically, should anchors/aliases/tags be opaque leaves or resolved? | Opaque = never wrong but useless to producers who use anchors; resolved = much more work and more failure modes. | Opaque, documented |
| **Q4** | Should the grammar accept a `---` frontmatter delimiter that is **not** at byte offset 0 (say, after leading blank lines)? | OKF §4 says "at the start of the file"; leniency diverges from the spec but matches real-world sloppiness. | No — spec-literal, host lint flags it |
| **Q5** | How should a `---` at column 0 **inside** a multi-line scalar be treated? | Genuinely ambiguous (§4.2). YAML says the indentation-sensitive content wins; getting it wrong silently truncates frontmatter. | Scalar state wins; needs a corpus test |
| **Q6** | What is the tab-width policy for OKF-YAML indentation? | Tabs in YAML indentation are illegal but must not crash. | Fixed width 8, documented, with a lint |
| **Q7** | Do we keep the `[[WikiLink]]` / `#tag` dialect rules at all, even build-gated? | Ecosystem pressure exists (`bitomb`); OKF v0.2 has none. Dead-but-present rules still cost maintenance. | Keep, gated, documented as experimental |
| **Q8** | Keep `section` (upstream-aligned) or flatten `body` to sibling blocks? | §7.1 notes the trade-off: alignment vs. trivial "the fence under `# Computation`" queries. Changes `docs/node-types.md`. | Keep `section`; solve §6.3 in queries |
| **Q9** | Should `okf_version` be an exception anywhere in the grammar? | It is the only frontmatter permitted in an `index.md` (OKF §12) — semantic and filename-dependent, so no node (D5). Confirm reviewers agree. | Query capture only |
| **Q10** | Do we ship the §7.5 host helpers as **reference implementations per binding**, or only as a written spec + shared fixtures? | Per-binding code is 5× the surface and drifts; a spec + fixtures risks 5 divergent behaviours anyway. | Spec + fixtures; one reference impl in the Node binding |
| **Q11** | Is `yaml_unsupported` acceptable as a public node name, given P7 semver-locks it? What's the deprecation path if the subset grows? | Once published, the name is API. A more neutral name (`yaml_opaque`?) leaves room. | Ask the reviewer |
| **Q12** | Which editors/hosts must work day one? | Decides whether D11's registration recipes and the ABI floor (D12) are P0 or P2. | Neovim + VS Code + a CLI |
| **Q13** | Is the performance target editor latency (small files, tiny edits) or bulk indexing (whole bundles)? | Different scanner tuning, different benches. | Editor latency |
| **Q14** | Dual-publish generated parsers per ABI (14 and 15), or single ABI? | Compatibility vs. release complexity. | Verify at M0 before committing |
| **Q15** | Do we vendor the official bundles as fixtures (Apache-2.0, attributed), or generate synthetic ones? | Vendoring gives real-world coverage but couples us to their layout and adds ~1 MB. | Vendor, pinned by SHA |

---

## 12. Milestones

| | Deliverable | Exit criteria |
|---|---|---|
| **M0** | Repo skeleton, `tree-sitter.json`, CI, vendored markdown merged and building, `MERGE.md` written | `tree-sitter generate` reproducible; upstream markdown corpus tests pass through our grammar |
| **M1** | Frontmatter: delimiters, recovery, mapping/sequence/flow, scalars, indentation scanner | Every frontmatter block in the fixtures parses; `parse --quiet` clean; §9.4(1) green |
| **M2** | Footnotes + OKF profile pinning + body fixtures | Zero `ERROR` on the fixture bundles |
| **M3** | Query library (§7.3); D4 pivot decision point | All query fixtures green in both frontmatter modes |
| **M4** | Property suite, differential suite, bench, fuzz corpus | §9.3 table green |
| **M5** | Bindings, docs, `docs/node-types.md`, release 1.0.0 | `npm i tree-sitter-okf && parse` works on a real bundle |

---

## Appendix A — Corpus evidence

Measured 2026-09-22 against the four official bundles at upstream `ad30107`. **Every number below is the output of `script/corpus-stats <bundle-dir>`, committed with this spec** — re-run it to check me. It separates frontmatter from body, and skips fenced code content, so these are the numbers the grammar actually has to parse.

**Totals: 78 markdown files, of which 54 have frontmatter and 24 do not.** All 24 frontmatter-less files are `index.md` (OKF §8). Note `acme_retail/log.md` **does** carry frontmatter (`type: Log`) — so "no frontmatter ⇒ index" is not a safe rule (§4.4).

**Frontmatter keys observed (16 distinct):**

| Key | Count | Key | Count |
|---|---|---|---|
| `type` | 54 | `stale_after` | 7 |
| `title` | 54 | `runtime` | 2 |
| `description` | 53 | `parameters` | 2 |
| `tags` | 53 | `executor` | 2 |
| `generated` | 53 | `attester` | 2 |
| `sources` | 49 | `usage_window` | 1 |
| `resource` | 47 | `not` *(producer-defined)* | 1 |
| `status` | 10 | `verified` | 8 |

**Frontmatter YAML forms observed** (707 key/value pairs at any indentation):

| Form | Count | Notes |
|---|---|---|
| plain scalar | 484 | includes values containing `&`, `:`, `(`, `)`, `,`, unbracketed `*` |
| empty value (`key:`) | 144 | every block collection starts this way |
| block sequence item | 140 | **129 of them at zero indentation** relative to their key — `tags:`, `sources:`, `verified:` |
| block sequence of mappings | 66 | the `not:` key in `metrics/gross-margin.md` |
| single-quoted scalar | 53 | mostly `at: '2026-07-10T21:15:20+00:00'` |
| multi-line scalar continuation line | 30 | folded plain scalars, e.g. `ga4/tables/events_.md` `description` |
| flow mapping | 21 | incl. nested in sequence items: `- { name: year, type: integer, required: true }` |
| flow sequence | 11 | `[job_id, executed_sql, result]` |
| double-quoted scalar | 4 | incl. long parenthesised strings |
| integer scalar | 1 | `usage_count: 1240` |

**Frontmatter forms *not* observed: zero** block scalars (`|`/`>`), anchors, aliases, tags, `#` comments, `...`, directives, or explicit keys — i.e. *every* item in §5.1 outside the subset is currently theoretical. This is the strongest available evidence for D2(c): the out-of-subset list is small and unexercised, while the never-fail requirement (P4) is the real one.

**Body constructs observed** (fenced content excluded):

| Construct | Count | Notes |
|---|---|---|
| inline code span | 509 | incl. **inside link text**: `` [`computations/revenue-ytd.md`](./revenue-ytd.md) `` |
| bullet list item | 367 | incl. nested-continuation items |
| pipe-table line | 213 | of which 10 are delimiter lines; cells contain `NUMERIC(18,4)`, `*emphasis*`, and footnote refs |
| strong emphasis | 139 | |
| ATX heading h1 / h2 / h3 | 129 / 27 / 21 | `h2` is dominated by `## YYYY-MM-DD` in `log.md` (OKF §9) |
| fenced code block | 85 opens | info strings: `sql` 81, bare 3, `json` 1; **7 fences are indented inside list items** |
| relative link | 74 | |
| non-ASCII body line | 47 | em-dashes, etc. |
| footnote reference | 45 | OKF §5.1 per-claim attribution |
| footnote definition | 34 | |
| absolute-URL link | 28 | |
| ordered list item | 22 | |
| bundle-absolute link | 8 | the upstream-recommended form (OKF §6.1) |
| emphasis | 43 | |

**Body constructs not observed: zero** wikilinks, `#`-hashtags, HTML blocks, HTML comments, block quotes, setext headings, autolinks, entities, images, hard line breaks, indented code blocks, link reference definitions, table-of-tables alignment rows.

Two of those need calling out, because "not observed" is not "not required":

* **Indented code blocks.** `script/corpus-stats` reports `indent4_body_line: 5`, but all five are *list-item continuation lines* (or SQL inside a fence), not indented code blocks — there are **zero** real indented code blocks in the bundles. They are nonetheless **required**, because OKF §10.2 and OKF §10.3 put the inline computation in a 4-space block. Hand-written fixtures only. This is exactly the kind of over-reporting the script warns about inline.
* **Hard line breaks.** The only two-space-trailing lines in the corpus are inside a SQL fence (trailing whitespace in generated SQL), not markdown hard breaks. Zero real ones.

These stay in the profile for CommonMark parity, but they carry **zero corpus coverage** and therefore need hand-written fixtures (§9.1 `body/`).

## Appendix B — Non-normative grammar sketch

Only the parts that are *not* vendored, or that illustrate a contentious decision.

```js
module.exports = grammar({
  name: 'okf',
  externals: $ => [
    $._frontmatter_open,   // '---' at byte 0 (BOM-tolerant), line-anchored   [D2, §5.2]
    $._frontmatter_close,  // '---' at col 0, not inside a scalar               [Q5]
    $._indent, $._dedent, $._same_indent,   // OKF-YAML indentation scanner   [§5.2]
    /* … vendored markdown scanner tokens … */
  ],
  extras: $ => [$._whitespace, $.comment, $.bom],

  rules: {
    source_file: $ => seq(
      optional(field('frontmatter', $.frontmatter)),
      field('body', $.body),
    ),

    frontmatter: $ => seq(
      $._frontmatter_open,
      optional($._yaml_content),
      optional($.yaml_document_end),              // '...'
      $._frontmatter_close,                       // MISSING-insertable        [D8]
    ),

    _yaml_content: $ => choice($.block_mapping, $.block_sequence, $.flow_node, $.yaml_unsupported),
    block_mapping: $ => seq($.block_mapping_pair, repeat(seq($._same_indent, $.block_mapping_pair))),
    block_mapping_pair: $ => seq(field('key', $._yaml_node), ':', optional(field('value', $._yaml_node))),
    block_sequence: $ => seq($.block_sequence_item, repeat(seq($._same_indent, $.block_sequence_item))),
    block_sequence_item: $ => seq('-', optional(field('value', $._yaml_node))),

    // D3: no scalar-type resolution at parse time.
    _yaml_node: $ => choice(
      $.block_mapping, $.block_sequence, $.flow_mapping, $.flow_sequence,
      $.plain_scalar, $.single_quote_scalar, $.double_quote_scalar,
      $.block_scalar, $.anchor, $.alias, $.tag,
    ),
    yaml_unsupported: _ => /[^\n]+/,              // never-fail escape hatch   [D2, §5.1]

    // D4: the body's block and inline rules are the vendored markdown rules, with
    // EXTENSION_PIPE_TABLE / GFM / STRIKETHROUGH / TASK_LIST pinned ON,
    // EXTENSION_MINUS_METADATA replaced by the rules above, and footnote
    // rules added.  Node names are upstream's (D10).
  },
});
```

## Appendix C — References

**Normative for the format**

* OKF v0.2 `SPEC.md` — <https://github.com/GoogleCloudPlatform/open-knowledge-format/blob/main/SPEC.md>
* OKF README and sample bundles — <https://github.com/GoogleCloudPlatform/open-knowledge-format>
* Frozen v0.1 snapshot (for the OKF §13 legacy fields) — `okf/` in <https://github.com/GoogleCloudPlatform/knowledge-catalog>

**Prior art**

* `tree-sitter-markdown` (block) + `tree-sitter-markdown-inline`, `common/common.js`, extension flags — <https://github.com/tree-sitter-grammars/tree-sitter-markdown> (npm 0.7.1)
* `tree-sitter-yaml` 0.5.0 — <https://github.com/tree-sitter-grammars/tree-sitter-yaml>
* `franklinbaldo/okf-parser` — <https://github.com/franklinbaldo/okf-parser>
* `mrqc/bitomb` (Pest, wiki-link dialect) — <https://github.com/mrqc/bitomb>
* Ecosystem index — <https://openknowledgeformat.com/ecosystem>
* CommonMark 0.30 and GFM (tables, footnotes) — <https://spec.commonmark.org/0.30/>, <https://github.github.com/gfm/>

**Measurement artefacts used for Appendix A**

* `script/corpus-stats` — committed with this spec; run it against `test/fixtures/bundles` after `script/sync-fixtures` clones the bundles at `ad30107`. Every table in this appendix is its output.
