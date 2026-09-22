#!/usr/bin/env node
/**
 * Shared host-helper fixtures (docs/host-helpers.md).
 *
 * Computes every helper's result for each case in test/helpers/cases.json
 * with the Node reference implementation and compares it with the case's
 * `expect`.  `--update` rewrites `expect` (review the diff by hand: it records
 * what the implementation does, not what OKF says it should do).  Other
 * bindings run the same cases against their own implementation.
 *
 * Usage: node script/helpers-fixtures.js [--update]
 */

'use strict';

const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert');
const Parser = require('tree-sitter');
const language = require('..');
const okf = require('../bindings/node/okf');

const ROOT = path.join(__dirname, '..');
const CASES = path.join(ROOT, 'test', 'helpers', 'cases.json');

function source(c) {
  if (c.source !== undefined) return c.source;
  return fs.readFileSync(path.join(ROOT, 'test', 'fixtures', 'bundles', c.fixture), 'utf8');
}

function evaluate(c) {
  const parser = new Parser();
  parser.setLanguage(language);
  const tree = parser.parse(source(c));
  const now = new Date(c.now || '2026-09-22T00:00:00Z');
  const cites = okf.citations(tree);
  return {
    classify: okf.classify(c.path, tree),
    conceptId: okf.conceptId(path.join('/bundle', c.path), '/bundle'),
    frontmatter: okf.frontmatter(tree),
    trustTier: okf.trustTier(tree),
    status: okf.status(tree),
    isStale: okf.isStale(tree, now),
    generatedAt: okf.generatedAt(tree),
    duplicateKeys: okf.duplicateKeys(tree),
    citations: {
      references: cites.references.map((r) => r.label),
      definitions: cites.definitions.map((d) => d.label),
      unresolved: cites.unresolved,
      unused: cites.unused,
    },
    legacyCitations: okf.legacyCitations(tree),
    conformance: okf.conformance(c.path, tree, { bundleRoot: !!c.bundleRoot }).map((f) => f.rule),
  };
}

function main() {
  const update = process.argv.includes('--update');
  const data = JSON.parse(fs.readFileSync(CASES, 'utf8'));
  let failures = 0;
  for (const c of data.cases) {
    const actual = evaluate(c);
    if (update) {
      c.expect = actual;
      continue;
    }
    try {
      assert.deepStrictEqual(actual, c.expect);
    } catch (e) {
      failures += 1;
      console.log(`DIFF  ${c.name}\n${e.message}\n`);
    }
  }
  if (update) {
    fs.writeFileSync(CASES, JSON.stringify(data, null, 1) + '\n');
    console.log(`updated ${data.cases.length} cases`);
    return 0;
  }
  console.log(`${data.cases.length} helper cases, ${failures} differing`);
  return failures ? 1 : 0;
}

if (require.main === module) process.exit(main());
module.exports = { evaluate };
