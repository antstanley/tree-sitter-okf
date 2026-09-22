"""OKF host helpers: the derived facts a query cannot express.

The grammar only records syntax (spec P3).  Everything that needs a filename,
a clock, the absence of a key or a value's meaning lives here, specified in
docs/host-helpers.md and pinned by the shared fixtures in
test/helpers/cases.json.  bindings/node/okf.js is the reference
implementation; this module follows it function for function.

Every function takes a tree-sitter ``Tree`` (or its root ``Node``) produced by
this grammar.
"""

from __future__ import annotations

import posixpath
import re
from datetime import datetime, timezone
from typing import Any, Optional

__all__ = [
    "classify",
    "concept_id",
    "frontmatter",
    "fields",
    "verified_entries",
    "trust_tier",
    "status",
    "is_stale",
    "generated_at",
    "duplicate_keys",
    "citations",
    "legacy_citations",
    "conformance",
]

_SKIPPED = ("comment", "yaml_directive", "yaml_document_end")


def _root(tree_or_node):
    return getattr(tree_or_node, "root_node", tree_or_node)


def _named(node):
    return [c for c in node.named_children if c.type not in _SKIPPED]


def _text(node) -> str:
    return node.text.decode("utf-8")


# ----------------------------------------------------------- classification


def classify(file_path: str, tree) -> str:
    """What kind of OKF document a file is (spec §4.4, OKF §3.1):
    ``index``, ``log``, ``concept`` or ``non_conformant``."""
    base = posixpath.basename(file_path.replace("\\", "/"))
    if base == "index.md":
        return "index"
    if base == "log.md":
        return "log"
    return "concept" if _root(tree).child_by_field_name("frontmatter") else "non_conformant"


def concept_id(file_path: str, bundle_root: str) -> str:
    """The path relative to the bundle root, without ``.md`` (spec §7.5)."""
    rel = posixpath.relpath(file_path.replace("\\", "/"), bundle_root.replace("\\", "/"))
    return re.sub(r"\.md$", "", rel)


# -------------------------------------------------------------- frontmatter


def _fold_flow(text: str) -> str:
    lines = text.split("\n")
    if len(lines) == 1:
        return text
    out = lines[0].rstrip(" \t")
    empties = 0
    for line in lines[1:]:
        stripped = line.strip()
        if stripped == "":
            empties += 1
            continue
        out += "\n" * empties if empties else " "
        out += stripped
        empties = 0
    return out + "\n" * empties


_ESCAPES = {
    "0": "\0", "a": "\a", "b": "\b", "t": "\t", "\t": "\t", "n": "\n", "v": "\v",
    "f": "\f", "r": "\r", "e": "\x1b", " ": " ", '"': '"', "/": "/", "\\": "\\",
    "N": "\x85", "_": "\xa0", "L": "\u2028", "P": "\u2029",
}


def _double_quoted(text: str) -> Optional[str]:
    body = text[1:-1]
    if re.search(r"\\\r?\n", body):
        return None  # out of subset: the grammar made it yaml_unsupported
    folded = _fold_flow(body.replace("\\\\", "\0\0")).replace("\0\0", "\\\\")
    out = []
    i = 0
    while i < len(folded):
        c = folded[i]
        if c == "\\" and i + 1 < len(folded):
            n = folded[i + 1]
            if n in _ESCAPES:
                out.append(_ESCAPES[n])
                i += 2
                continue
            width = {"x": 2, "u": 4, "U": 8}.get(n)
            if width:
                out.append(chr(int(folded[i + 2:i + 2 + width], 16)))
                i += 2 + width
                continue
        out.append(c)
        i += 1
    return "".join(out)


def _source_bytes(node) -> bytes:
    root = node
    while root.parent is not None:
        root = root.parent
    return root.text


def _parent_indentation(node) -> int:
    """The indentation of the line a node starts on: a block scalar's
    explicit indentation indicator counts from there."""
    src = _source_bytes(node)
    line_start = src.rfind(b"\n", 0, node.start_byte) + 1
    line = src[line_start:node.start_byte]
    return len(line) - len(line.lstrip(b" "))


def _fold_block(lines):
    out = ""
    prev_normal = False
    empties = 0
    for i, line in enumerate(lines):
        if line == "":
            empties += 1
            continue
        more_indented = line[:1] in (" ", "\t")
        if (i and out != "") or empties:
            if prev_normal and not more_indented and empties == 0:
                out += " "
            elif prev_normal and not more_indented:
                out += "\n" * empties
            else:
                out += "\n" * (empties + 1) if out != "" else "\n" * empties
        out += line
        prev_normal = not more_indented
        empties = 0
    return out


def _block_scalar(node) -> str:
    text = _text(node)
    header, _, body = text.partition("\n")
    style = header[0]
    indicators = header[1:].split("#")[0].strip()
    chomp = "-" if "-" in indicators else "+" if "+" in indicators else ""
    digits = [c for c in indicators if c in "123456789"]
    lines = body.split("\n") if body else []
    if digits:
        indent = _parent_indentation(node) + int(digits[0])
    else:
        indents = [len(l) - len(l.lstrip(" ")) for l in lines if l.strip()]
        indent = min(indents) if indents else 0
    content = [l[indent:] if l.strip() else "" for l in lines]
    rest = _source_bytes(node)[node.end_byte:].decode("utf-8").split("\n")[1:]
    trailing = 0
    for line in rest:
        if line.strip() == "":
            trailing += 1
        else:
            break
    value = "\n".join(content) if style == "|" else _fold_block(content)
    if chomp == "-":
        return value.rstrip("\n")
    if chomp == "+":
        return value + "\n" + "\n" * trailing if value else "\n" * trailing
    return value + "\n" if value else ""


class _ValueBuilder:
    """Builds values best-effort: content outside OKF-YAML is left out of
    mappings and sequences and becomes None elsewhere, and ``unsupported``
    records that it happened."""

    def __init__(self):
        self.anchors = {}
        self.unsupported = False

    def sequence_value(self, nodes):
        anchor = None
        result = None
        seen = False
        for n in nodes:
            if n.type == "anchor":
                anchor = _text(n)[1:]
            elif n.type == "tag":
                continue
            else:
                result = self.node(n)
                seen = True
        if not seen:
            result = None
        if anchor is not None:
            self.anchors[anchor] = result
        return result

    def node(self, n):
        t = n.type
        if t == "yaml_unsupported":
            self.unsupported = True
            return None
        if t == "plain_scalar":
            return _fold_flow(_text(n))
        if t == "single_quote_scalar":
            return _fold_flow(_text(n)[1:-1]).replace("''", "'")
        if t == "double_quote_scalar":
            return _double_quoted(_text(n))
        if t == "block_scalar":
            return _block_scalar(n)
        if t == "alias":
            return self.anchors.get(_text(n)[1:])
        if t == "block_mapping":
            out = {}
            for pair in _named(n):
                if pair.type != "block_mapping_pair":
                    self.unsupported = True
                    continue
                key = pair.child_by_field_name("key")
                value = pair.child_by_field_name("value")
                props = [c for c in _named(pair) if c.type in ("anchor", "tag")]
                out[self.node(key)] = self.sequence_value(props + ([value] if value is not None else []))
            return out
        if t == "block_sequence":
            out = []
            for item in _named(n):
                if item.type != "block_sequence_item":
                    self.unsupported = True
                    continue
                out.append(self.sequence_value(_named(item)))
            return out
        if t == "flow_mapping":
            out = {}
            for entry in _named(n):
                if entry.type == "flow_pair":
                    value = entry.child_by_field_name("value")
                    out[self.node(entry.child_by_field_name("key"))] = (
                        self.node(value) if value is not None else None
                    )
                elif entry.type not in ("anchor", "tag"):
                    out[self.node(entry)] = None
            return out
        if t == "flow_sequence":
            out = []
            pending = []
            for entry in _named(n):
                if entry.type in ("anchor", "tag"):
                    pending.append(entry)
                    continue
                if entry.type == "flow_pair":
                    value = entry.child_by_field_name("value")
                    out.append({self.node(entry.child_by_field_name("key")):
                                self.node(value) if value is not None else None})
                else:
                    out.append(self.sequence_value(pending + [entry]))
                pending = []
            return out
        self.unsupported = True
        return None


def frontmatter(tree) -> dict:
    """The frontmatter as a plain value (spec D3: scalars are never typed).

    Returns ``{"present", "closed", "value", "unsupported"}``; see
    bindings/node/okf.js ``frontmatter`` for the contract."""
    fm = _root(tree).child_by_field_name("frontmatter")
    if fm is None:
        return {"present": False, "closed": False, "value": None, "unsupported": False}
    delimiters = [c for c in fm.children if c.type == "---"]
    closed = len(delimiters) == 2 and not delimiters[1].is_missing
    builder = _ValueBuilder()
    nodes = []
    for c in _named(fm):
        if c.type == "yaml_unsupported":
            builder.unsupported = True
        else:
            nodes.append(c)
    value = builder.sequence_value(nodes) if nodes else None
    return {"present": True, "closed": closed, "value": value, "unsupported": builder.unsupported}


def fields(tree) -> dict:
    value = frontmatter(tree)["value"]
    return value if isinstance(value, dict) else {}


# ------------------------------------------------------------ derived facts


def verified_entries(tree) -> list:
    """``verified`` as a list (OKF §5.2: a bare mapping is a one-element list)."""
    verified = fields(tree).get("verified")
    if verified is None:
        return []
    entries = verified if isinstance(verified, list) else [verified]
    return [e for e in entries if isinstance(e, dict)]


def trust_tier(tree) -> str:
    """``unverified``, ``machine-confirmed`` or ``human-reviewed`` (OKF §5.3)."""
    if "verified" not in fields(tree):
        return "unverified"
    for e in verified_entries(tree):
        by = e.get("by")
        if isinstance(by, str) and by.startswith("human:"):
            return "human-reviewed"
    return "machine-confirmed"


def status(tree) -> str:
    """The lifecycle status (OKF §5.4): absent means ``stable``."""
    s = fields(tree).get("status")
    return s if isinstance(s, str) and s != "" else "stable"


def _instant(value: str) -> Optional[datetime]:
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed


def is_stale(tree, now: Optional[datetime] = None) -> Optional[bool]:
    """``now >= stale_after`` (OKF §5.5); None without a valid ``stale_after``."""
    value = fields(tree).get("stale_after")
    if not isinstance(value, str):
        return None
    at = _instant(value)
    if at is None:
        return None
    now = now or datetime.now(timezone.utc)
    return now >= at


def generated_at(tree) -> Optional[str]:
    """``generated.at``, falling back to the v0.1 ``timestamp`` (OKF §13.1)."""
    f = fields(tree)
    generated = f.get("generated")
    if isinstance(generated, dict) and isinstance(generated.get("at"), str):
        return generated["at"]
    if "generated" not in f and isinstance(f.get("timestamp"), str):
        return f["timestamp"]
    return None


def duplicate_keys(tree) -> list:
    """Keys occurring more than once in one frontmatter mapping (spec §5.4)."""
    fm = _root(tree).child_by_field_name("frontmatter")
    found = []
    if fm is None:
        return found
    builder = _ValueBuilder()

    def walk(node, trail):
        if node.type in ("block_mapping", "flow_mapping"):
            counts = {}
            for pair in _named(node):
                if pair.type not in ("block_mapping_pair", "flow_pair"):
                    continue
                key = builder.node(pair.child_by_field_name("key"))
                counts[key] = counts.get(key, 0) + 1
                value = pair.child_by_field_name("value")
                if value is not None:
                    walk(value, trail + [key])
            for key, count in counts.items():
                if count > 1:
                    found.append({"key": key, "count": count, "path": trail})
        elif node.type in ("block_sequence", "flow_sequence"):
            for i, item in enumerate(_named(node)):
                for child in _named(item):
                    walk(child, trail + [i])
        else:
            for child in _named(node):
                walk(child, trail)

    for child in _named(fm):
        walk(child, [])
    return found


def citations(tree) -> dict:
    """The ``sources[].id`` / footnote label join (OKF §5.1)."""
    root = _root(tree)
    sources = fields(tree).get("sources")
    sources = sources if isinstance(sources, list) else []
    references, definitions = [], []

    def walk(node):
        if node.type in ("footnote_reference", "footnote_definition"):
            label = node.child_by_field_name("label")
            if label is not None:
                entry = {"label": _text(label), "row": label.start_point[0],
                         "column": label.start_point[1]}
                (references if node.type == "footnote_reference" else definitions).append(entry)
        for child in node.named_children:
            walk(child)

    body = root.child_by_field_name("body")
    if body is not None:
        walk(body)
    ids = []
    for s in sources:
        if isinstance(s, dict) and isinstance(s.get("id"), str) and s["id"] not in ids:
            ids.append(s["id"])
    referenced = []
    for r in references:
        if r["label"] not in referenced:
            referenced.append(r["label"])
    return {
        "sources": sources,
        "references": references,
        "definitions": definitions,
        "unresolved": [label for label in referenced if label not in ids],
        "unused": [i for i in ids if i not in referenced],
    }


def legacy_citations(tree) -> Optional[list]:
    """The v0.1 ``# Citations`` body list (OKF §13.1), or None."""
    body = _root(tree).child_by_field_name("body")
    if body is None:
        return None
    for section in body.named_children:
        if section.type != "section":
            continue
        heading = next((c for c in section.named_children if c.type == "atx_heading"), None)
        content = heading.child_by_field_name("heading_content") if heading else None
        if content is None or not re.match(r"^citations\s*$", _text(content), re.I):
            continue
        items = []
        for block in section.named_children:
            if block.type != "list":
                continue
            for item in block.named_children:
                paragraph = next((c for c in item.named_children if c.type == "paragraph"), None)
                if paragraph is not None:
                    items.append(_text(paragraph).strip())
        return items
    return None


# -------------------------------------------------------------- conformance


def conformance(file_path: str, tree, bundle_root: bool = False) -> list:
    """Conformance findings for one document (OKF §11): a list of
    ``{"rule", "severity", "message"}``.  Advice, never a rejection."""
    findings = []
    kind = classify(file_path, tree)
    root = _root(tree)
    fm = frontmatter(tree)

    def add(rule, severity, message):
        findings.append({"rule": rule, "severity": severity, "message": message})

    if kind in ("concept", "non_conformant"):
        if not fm["present"]:
            add("frontmatter-required", "error",
                "a concept document needs a frontmatter block (OKF §11.1)")
        elif not fm["closed"]:
            add("frontmatter-unterminated", "error",
                "the frontmatter block has no closing `---` (OKF §11.1)")
        else:
            value = fm["value"]
            kind_type = value.get("type") if isinstance(value, dict) else None
            if not isinstance(kind_type, str) or kind_type.strip() == "":
                add("type-required", "error", "the frontmatter needs a non-empty `type` (OKF §11.2)")
    if kind == "index" and fm["present"]:
        value = fm["value"]
        keys = list(value.keys()) if isinstance(value, dict) else []
        if not (bundle_root and keys == ["okf_version"]):
            add("index-frontmatter", "error",
                "index.md has no frontmatter, except `okf_version` in the bundle root (OKF §8, §12)")
    if kind == "log":
        def walk(node):
            if node.type == "atx_heading" and any(c.type == "atx_h2_marker" for c in node.named_children):
                content = node.child_by_field_name("heading_content")
                text = _text(content).strip() if content is not None else ""
                if not re.match(r"^\d{4}-\d{2}-\d{2}$", text):
                    add("log-date-heading", "error",
                        f'log date headings use ISO 8601 YYYY-MM-DD (OKF §9): "{text}"')
            for child in node.named_children:
                walk(child)

        body = root.child_by_field_name("body")
        if body is not None:
            walk(body)
    if fm["present"] and fm["unsupported"]:
        add("frontmatter-outside-subset", "warning",
            "part of the frontmatter is outside OKF-YAML (docs/okf-yaml.md)")
    if root.has_error and not (fm["present"] and not fm["closed"]):
        add("syntax", "warning", "the document has syntax the grammar could not parse")
    return findings
