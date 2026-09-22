/**
 * @file OKF host helpers: the derived facts a query cannot express.
 * @license MIT
 *
 * The grammar only records syntax (spec P3).  Everything that needs a
 * filename, a clock, the absence of a key or a value's meaning lives here,
 * specified in docs/host-helpers.md and pinned by the shared fixtures in
 * test/helpers/cases.json, so every binding that implements them behaves
 * identically (spec §4.4, §7.5, Q10).  This file is the reference
 * implementation; bindings/python/tree_sitter_okf/okf.py is the second.
 *
 * Every function takes a tree-sitter `Tree` (or its root node) produced by
 * this grammar, plus the source text where values are needed.
 */

'use strict';

const path = require('node:path');

/* ------------------------------------------------------------------ helpers */

function rootOf(treeOrNode) {
  return treeOrNode && treeOrNode.rootNode ? treeOrNode.rootNode : treeOrNode;
}

function namedChildren(node) {
  return node.namedChildren.filter(
    (c) => c.type !== 'comment' && c.type !== 'yaml_directive' && c.type !== 'yaml_document_end',
  );
}

/* --------------------------------------------------------- classification */

/**
 * What kind of OKF document a file is (spec §4.4, OKF §3.1).  The grammar
 * cannot tell: it depends on the filename.
 *
 * @param {string} filePath
 * @param {object} tree
 * @returns {'index'|'log'|'concept'|'non_conformant'}
 */
function classify(filePath, tree) {
  const base = path.posix.basename(filePath.replace(/\\/g, '/'));
  if (base === 'index.md') return 'index';
  if (base === 'log.md') return 'log';
  const root = rootOf(tree);
  return root.childForFieldName('frontmatter') ? 'concept' : 'non_conformant';
}

/**
 * The concept id: the path relative to the bundle root, without `.md`, with
 * forward slashes (spec §7.5).
 */
function conceptId(filePath, bundleRoot) {
  const rel = path.relative(bundleRoot, filePath).split(path.sep).join('/');
  return rel.replace(/\.md$/, '');
}

/* ------------------------------------------------------------ frontmatter */

// YAML flow folding for plain and quoted scalars: lines are trimmed, a single
// line break becomes a space, each empty line a newline.
function foldFlow(text) {
  const lines = text.split('\n');
  if (lines.length === 1) return text;
  let out = lines[0].replace(/[ \t]+$/, '');
  let empties = 0;
  for (const line of lines.slice(1)) {
    const stripped = line.trim();
    if (stripped === '') {
      empties += 1;
      continue;
    }
    out += empties ? '\n'.repeat(empties) : ' ';
    out += stripped;
    empties = 0;
  }
  return out + '\n'.repeat(empties);
}

const ESCAPES = {
  '0': '\0', a: '\x07', b: '\b', t: '\t', '\t': '\t', n: '\n', v: '\v', f: '\f', r: '\r',
  e: '\x1b', ' ': ' ', '"': '"', '/': '/', '\\': '\\', N: '\x85', _: '\xa0',
  L: '\u2028', P: '\u2029',
};

function doubleQuotedValue(text) {
  const body = text.slice(1, -1);
  if (/\\\r?\n/.test(body)) return null; // out of subset: the grammar made it yaml_unsupported
  const folded = foldFlow(body.replace(/\\\\/g, '\0\0')).replace(/\0\0/g, '\\\\');
  let out = '';
  for (let i = 0; i < folded.length; i++) {
    const c = folded[i];
    if (c === '\\' && i + 1 < folded.length) {
      const n = folded[i + 1];
      if (n in ESCAPES) {
        out += ESCAPES[n];
        i += 1;
        continue;
      }
      const width = { x: 2, u: 4, U: 8 }[n];
      if (width) {
        out += String.fromCodePoint(parseInt(folded.slice(i + 2, i + 2 + width), 16));
        i += 1 + width;
        continue;
      }
    }
    out += c;
  }
  return out;
}

// The indentation of the line a node starts on: a block scalar's explicit
// indentation indicator counts from there.
function parentIndentation(node) {
  const tree = node.tree;
  const text = tree.rootNode.text;
  const start = node.startIndex;
  const lineStart = text.lastIndexOf('\n', start - 1) + 1;
  const line = text.slice(lineStart, start);
  return line.length - line.replace(/^ +/, '').length;
}

function foldBlock(lines) {
  let out = '';
  let prevNormal = false;
  let empties = 0;
  lines.forEach((line, i) => {
    if (line === '') {
      empties += 1;
      return;
    }
    const moreIndented = line[0] === ' ' || line[0] === '\t';
    if ((i && out !== '') || empties) {
      if (prevNormal && !moreIndented && empties === 0) out += ' ';
      else if (prevNormal && !moreIndented) out += '\n'.repeat(empties);
      else out += out !== '' ? '\n'.repeat(empties + 1) : '\n'.repeat(empties);
    }
    out += line;
    prevNormal = !moreIndented;
    empties = 0;
  });
  return out;
}

function blockScalarValue(node) {
  const text = node.text;
  const newline = text.indexOf('\n');
  const header = newline < 0 ? text : text.slice(0, newline);
  const body = newline < 0 ? '' : text.slice(newline + 1);
  const style = header[0];
  const indicators = header.slice(1).split('#')[0].trim();
  const chomp = indicators.includes('-') ? '-' : indicators.includes('+') ? '+' : '';
  const digit = (indicators.match(/[1-9]/) || [])[0];
  const lines = body ? body.split('\n') : [];
  let indent;
  if (digit) {
    indent = parentIndentation(node) + Number(digit);
  } else {
    const indents = lines.filter((l) => l.trim()).map((l) => l.length - l.replace(/^ +/, '').length);
    indent = indents.length ? Math.min(...indents) : 0;
  }
  const content = lines.map((l) => (l.trim() ? l.slice(indent) : ''));
  // Trailing blank lines are not part of the token; they matter for `keep`.
  const whole = node.tree.rootNode.text;
  const rest = whole.slice(node.endIndex).split('\n').slice(1);
  let trailing = 0;
  for (const line of rest) {
    if (line.trim() === '') trailing += 1;
    else break;
  }
  const value = style === '|' ? content.join('\n') : foldBlock(content);
  if (chomp === '-') return value.replace(/\n+$/, '');
  if (chomp === '+') return value ? value + '\n' + '\n'.repeat(trailing) : '\n'.repeat(trailing);
  return value ? value + '\n' : '';
}

// Builds values best-effort: content outside OKF-YAML is left out of
// mappings and sequences and becomes null elsewhere, and `unsupported`
// records that it happened.
class ValueBuilder {
  constructor() {
    this.anchors = new Map();
    this.unsupported = false;
  }

  sequenceValue(nodes) {
    let anchor = null;
    let result;
    let seen = false;
    for (const n of nodes) {
      if (n.type === 'anchor') anchor = n.text.slice(1);
      else if (n.type === 'tag') continue;
      else {
        result = this.node(n);
        seen = true;
      }
    }
    if (!seen) result = null;
    if (anchor !== null) this.anchors.set(anchor, result);
    return result;
  }

  node(n) {
    switch (n.type) {
      case 'yaml_unsupported':
        this.unsupported = true;
        return null;
      case 'plain_scalar':
        return foldFlow(n.text);
      case 'single_quote_scalar':
        return foldFlow(n.text.slice(1, -1)).replace(/''/g, "'");
      case 'double_quote_scalar':
        return doubleQuotedValue(n.text);
      case 'block_scalar':
        return blockScalarValue(n);
      case 'alias':
        return this.anchors.has(n.text.slice(1)) ? this.anchors.get(n.text.slice(1)) : null;
      case 'block_mapping': {
        const out = {};
        for (const pair of namedChildren(n)) {
          if (pair.type !== 'block_mapping_pair') {
            this.unsupported = true;
            continue;
          }
          const keyNode = pair.childForFieldName('key');
          const valueNode = pair.childForFieldName('value');
          const props = namedChildren(pair).filter((c) => c.type === 'anchor' || c.type === 'tag');
          out[this.node(keyNode)] = this.sequenceValue(valueNode ? [...props, valueNode] : props);
        }
        return out;
      }
      case 'block_sequence':
        return namedChildren(n)
          .filter((item) => {
            if (item.type === 'block_sequence_item') return true;
            this.unsupported = true;
            return false;
          })
          .map((item) => this.sequenceValue(namedChildren(item)));
      case 'flow_mapping': {
        const out = {};
        for (const entry of namedChildren(n)) {
          if (entry.type === 'flow_pair') {
            const value = entry.childForFieldName('value');
            out[this.node(entry.childForFieldName('key'))] = value ? this.node(value) : null;
          } else if (entry.type !== 'anchor' && entry.type !== 'tag') {
            out[this.node(entry)] = null;
          }
        }
        return out;
      }
      case 'flow_sequence': {
        const out = [];
        let pending = [];
        for (const entry of namedChildren(n)) {
          if (entry.type === 'anchor' || entry.type === 'tag') {
            pending.push(entry);
            continue;
          }
          if (entry.type === 'flow_pair') {
            const value = entry.childForFieldName('value');
            out.push({ [this.node(entry.childForFieldName('key'))]: value ? this.node(value) : null });
          } else {
            out.push(this.sequenceValue([...pending, entry]));
          }
          pending = [];
        }
        return out;
      }
      default:
        this.unsupported = true;
        return null;
    }
  }
}

/**
 * The frontmatter as a plain value: mappings become objects, sequences arrays,
 * scalars strings (never typed, spec D3), and an empty value `null`.
 *
 * @returns {{present: boolean, closed: boolean, value: any, unsupported: boolean}}
 *   `present` is false with no frontmatter; `closed` false when the closing
 *   `---` is missing; `unsupported` true when some content is outside
 *   OKF-YAML.  The value is then best-effort: unsupported entries are left
 *   out and unsupported values are null (the grammar does not claim to know
 *   them).
 */
function frontmatter(tree) {
  const fm = rootOf(tree).childForFieldName('frontmatter');
  if (!fm) return { present: false, closed: false, value: null, unsupported: false };
  const delimiters = fm.children.filter((c) => c.type === '---');
  const closed = delimiters.length === 2 && !delimiters[1].isMissing;
  // Unsupported lines before the root node are siblings of it.
  const content = namedChildren(fm);
  const builder = new ValueBuilder();
  const nodes = content.filter((c) => {
    if (c.type !== 'yaml_unsupported') return true;
    builder.unsupported = true;
    return false;
  });
  const value = nodes.length ? builder.sequenceValue(nodes) : null;
  return { present: true, closed, value, unsupported: builder.unsupported };
}

/** The frontmatter value if it is a mapping, else an empty object. */
function fields(tree) {
  const { value } = frontmatter(tree);
  return value && typeof value === 'object' && !Array.isArray(value) ? value : {};
}

/* ------------------------------------------------------------ derived facts */

/**
 * `verified` as a list (OKF §5.2: a bare mapping is a one-element list).
 * Entries that are not mappings are kept out.
 */
function verifiedEntries(tree) {
  const verified = fields(tree).verified;
  if (verified === undefined || verified === null) return [];
  const list = Array.isArray(verified) ? verified : [verified];
  return list.filter((e) => e && typeof e === 'object' && !Array.isArray(e));
}

/**
 * The trust tier (OKF §5.3): 'unverified' without a `verified` key,
 * 'human-reviewed' when any verifier is a `human:` actor, else
 * 'machine-confirmed'.
 */
function trustTier(tree) {
  if (!('verified' in fields(tree))) return 'unverified';
  const entries = verifiedEntries(tree);
  if (entries.some((e) => typeof e.by === 'string' && e.by.startsWith('human:'))) {
    return 'human-reviewed';
  }
  return 'machine-confirmed';
}

/** The lifecycle status (OKF §5.4): absent means 'stable'. */
function status(tree) {
  const s = fields(tree).status;
  return typeof s === 'string' && s !== '' ? s : 'stable';
}

/**
 * Whether the concept is stale at `now` (OKF §5.5): `now >= stale_after`.
 * Returns null when there is no `stale_after` or it is not a valid instant.
 */
function isStale(tree, now = new Date()) {
  const value = fields(tree).stale_after;
  if (typeof value !== 'string') return null;
  const at = Date.parse(value);
  if (Number.isNaN(at)) return null;
  return now.getTime() >= at;
}

/**
 * When the content last changed: `generated.at`, falling back to the v0.1
 * `timestamp` key when `generated` is absent (OKF §13.1).
 */
function generatedAt(tree) {
  const f = fields(tree);
  if (f.generated && typeof f.generated === 'object' && typeof f.generated.at === 'string') {
    return f.generated.at;
  }
  if (!('generated' in f) && typeof f.timestamp === 'string') return f.timestamp;
  return null;
}

/**
 * Keys that occur more than once in the same frontmatter mapping (spec §5.4:
 * parsed, never an error; a linter decides).  Returns [{key, count, path}].
 */
function duplicateKeys(tree) {
  const fm = rootOf(tree).childForFieldName('frontmatter');
  const found = [];
  if (!fm) return found;
  const builder = new ValueBuilder();
  const walk = (node, trail) => {
    if (node.type === 'block_mapping' || node.type === 'flow_mapping') {
      const counts = new Map();
      for (const pair of namedChildren(node)) {
        if (pair.type !== 'block_mapping_pair' && pair.type !== 'flow_pair') continue;
        const key = builder.node(pair.childForFieldName('key'));
        counts.set(key, (counts.get(key) || 0) + 1);
        const value = pair.childForFieldName('value');
        if (value) walk(value, [...trail, key]);
      }
      for (const [key, count] of counts) {
        if (count > 1) found.push({ key, count, path: trail });
      }
    } else if (node.type === 'block_sequence' || node.type === 'flow_sequence') {
      namedChildren(node).forEach((item, i) => {
        for (const child of namedChildren(item)) walk(child, [...trail, i]);
      });
    } else {
      for (const child of namedChildren(node)) walk(child, trail);
    }
  };
  for (const child of namedChildren(fm)) walk(child, []);
  return found;
}

/**
 * Per-claim attribution (OKF §5.1): the join between frontmatter
 * `sources[].id` and body footnote labels.
 *
 * @returns {{sources: object[], references: {label, row, column}[],
 *   definitions: {label, row, column}[], unresolved: string[], unused: string[]}}
 *   `unresolved` are labels referenced in the body with no source of that id;
 *   `unused` are source ids no footnote references.
 */
function citations(tree) {
  const root = rootOf(tree);
  const sources = Array.isArray(fields(tree).sources) ? fields(tree).sources : [];
  const references = [];
  const definitions = [];
  const walk = (node) => {
    if (node.type === 'footnote_reference' || node.type === 'footnote_definition') {
      const label = node.childForFieldName('label');
      if (label) {
        const entry = { label: label.text, row: label.startPosition.row, column: label.startPosition.column };
        (node.type === 'footnote_reference' ? references : definitions).push(entry);
      }
    }
    for (const child of node.namedChildren) walk(child);
  };
  const body = root.childForFieldName('body');
  if (body) walk(body);
  const ids = new Set(
    sources.filter((s) => s && typeof s === 'object' && typeof s.id === 'string').map((s) => s.id),
  );
  const referenced = new Set(references.map((r) => r.label));
  return {
    sources,
    references,
    definitions,
    unresolved: [...referenced].filter((label) => !ids.has(label)),
    unused: [...ids].filter((id) => !referenced.has(id)),
  };
}

/**
 * The v0.1 `# Citations` body list, for documents without `sources`
 * (OKF §13.1).  Returns the text of each list item under a heading named
 * "Citations", or null when there is none.
 */
function legacyCitations(tree) {
  const body = rootOf(tree).childForFieldName('body');
  if (!body) return null;
  for (const section of body.namedChildren) {
    if (section.type !== 'section') continue;
    const heading = section.namedChildren.find((c) => c.type === 'atx_heading');
    const content = heading && heading.childForFieldName('heading_content');
    if (!content || !/^citations\s*$/i.test(content.text)) continue;
    const items = [];
    for (const block of section.namedChildren) {
      if (block.type !== 'list') continue;
      for (const item of block.namedChildren) {
        const paragraph = item.namedChildren.find((c) => c.type === 'paragraph');
        if (paragraph) items.push(paragraph.text.trim());
      }
    }
    return items;
  }
  return null;
}

/* ------------------------------------------------------------- conformance */

/**
 * Conformance findings for one document (OKF §11).  Findings are advice:
 * OKF forbids rejecting a bundle for most of them (§11).
 *
 * @param {string} filePath
 * @param {object} tree
 * @param {{bundleRoot?: boolean}} [options] `bundleRoot`: whether the file
 *   is in the bundle root (only a bundle-root index.md may carry
 *   `okf_version`).
 * @returns {{rule: string, severity: 'error'|'warning', message: string}[]}
 */
function conformance(filePath, tree, options = {}) {
  const findings = [];
  const kind = classify(filePath, tree);
  const root = rootOf(tree);
  const fm = frontmatter(tree);

  if (kind === 'concept' || kind === 'non_conformant') {
    if (!fm.present) {
      findings.push({ rule: 'frontmatter-required', severity: 'error',
        message: 'a concept document needs a frontmatter block (OKF §11.1)' });
    } else if (!fm.closed) {
      findings.push({ rule: 'frontmatter-unterminated', severity: 'error',
        message: 'the frontmatter block has no closing `---` (OKF §11.1)' });
    } else {
      const type = fm.value && typeof fm.value === 'object' ? fm.value.type : undefined;
      if (typeof type !== 'string' || type.trim() === '') {
        findings.push({ rule: 'type-required', severity: 'error',
          message: 'the frontmatter needs a non-empty `type` (OKF §11.2)' });
      }
    }
  }
  if (kind === 'index' && fm.present) {
    const keys = fm.value && typeof fm.value === 'object' ? Object.keys(fm.value) : [];
    const onlyVersion = keys.length === 1 && keys[0] === 'okf_version';
    if (!(options.bundleRoot && onlyVersion)) {
      findings.push({ rule: 'index-frontmatter', severity: 'error',
        message: 'index.md has no frontmatter, except `okf_version` in the bundle root (OKF §8, §12)' });
    }
  }
  if (kind === 'log') {
    const body = root.childForFieldName('body');
    const walk = (node) => {
      if (node.type === 'atx_heading' && node.namedChildren.some((c) => c.type === 'atx_h2_marker')) {
        const content = node.childForFieldName('heading_content');
        const text = content ? content.text.trim() : '';
        if (!/^\d{4}-\d{2}-\d{2}$/.test(text)) {
          findings.push({ rule: 'log-date-heading', severity: 'error',
            message: `log date headings use ISO 8601 YYYY-MM-DD (OKF §9): "${text}"` });
        }
      }
      for (const child of node.namedChildren) walk(child);
    };
    if (body) walk(body);
  }
  if (fm.present && fm.unsupported) {
    findings.push({ rule: 'frontmatter-outside-subset', severity: 'warning',
      message: 'part of the frontmatter is outside OKF-YAML (docs/okf-yaml.md)' });
  }
  if (root.hasError && !(fm.present && !fm.closed)) {
    findings.push({ rule: 'syntax', severity: 'warning',
      message: 'the document has syntax the grammar could not parse' });
  }
  return findings;
}

module.exports = {
  classify,
  conceptId,
  frontmatter,
  fields,
  verifiedEntries,
  trustTier,
  status,
  isStale,
  generatedAt,
  duplicateKeys,
  citations,
  legacyCitations,
  conformance,
};
