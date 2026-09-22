---
"tree-sitter-okf": patch
---

Fix the defects found in the 1.0.0 code review.

Frontmatter parsing:

- A key after an anchor or tag (`&a key: v`) no longer shifts the indentation of the lines after it.
- An indented `%` line is content (`yaml_unsupported`), not a directive, so it can no longer swallow the closing `---` and the body.
- Nesting deeper than 48 levels makes the extra lines `yaml_unsupported` instead of turning the whole document into `ERROR`.
- Tabs count to the next multiple of 8 everywhere on a line, so tab-indented compact mappings and roots keep their siblings.
- A line shallower than an indented root is `yaml_unsupported` instead of a sibling.

Markdown parsing:

- A long run of `*`, `_` or `~` that nothing closes is now parsed in linear time; it was quadratic.

Host helpers (Node and Python):

- `\x`, `\u` and `\U` escapes that don't name a code point (such as `"C:\Users"`) are kept as written; the helpers no longer throw on them.
- An escaped backslash at the end of a line is no longer read as an escaped line break.
- CRLF documents no longer leave `\r` in values.
- `isStale` accepts only ISO 8601 and reads a date-time without an offset as UTC, identically in both ports.
- Node: `__proto__` and `constructor` are ordinary keys.

Release workflow: the npm version in the publishing job is pinned, and a re-run while a version is staged but not yet approved no longer fails.
