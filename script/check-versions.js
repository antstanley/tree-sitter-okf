#!/usr/bin/env node
/**
 * Check that every manifest carries the same version (spec §10.3).
 *
 * The version lives in eight files; script/version-packages keeps them in
 * step.  This fails if any disagree with package.json, or, given a tag
 * (`v1.2.3` or `1.2.3`), with that tag: the release workflow runs it so a tag
 * can never publish a mismatched set.
 *
 * Usage: node script/check-versions.js [TAG]
 */

'use strict';

const fs = require('node:fs');
const path = require('node:path');

const ROOT = path.join(__dirname, '..');
const read = (file) => fs.readFileSync(path.join(ROOT, file), 'utf8');

function match(file, regex) {
  const m = read(file).match(regex);
  return m ? m[1] : null;
}

const pkg = JSON.parse(read('package.json'));
const lock = JSON.parse(read('package-lock.json'));

const found = {
  'package.json': pkg.version,
  'package-lock.json': lock.version,
  'package-lock.json (root package)': lock.packages && lock.packages[''] && lock.packages[''].version,
  'tree-sitter.json': JSON.parse(read('tree-sitter.json')).metadata.version,
  'Cargo.toml': match('Cargo.toml', /^\[package\][^[]*?^version\s*=\s*"([^"]+)"/ms),
  'Cargo.lock': match('Cargo.lock', /name = "tree-sitter-okf"\nversion = "([^"]+)"/),
  'pyproject.toml': match('pyproject.toml', /^\[project\][^[]*?^version\s*=\s*"([^"]+)"/ms),
  Makefile: match('Makefile', /^VERSION := (\S+)/m),
  'CMakeLists.txt': match('CMakeLists.txt', /VERSION "([^"]+)"/),
};

const tag = process.argv[2];
const expected = tag ? tag.replace(/^v/, '') : pkg.version;
let failed = false;
for (const [file, version] of Object.entries(found)) {
  if (version !== expected) {
    console.error(`${file}: ${version ?? '(no version found)'}, expected ${expected}`);
    failed = true;
  }
}
if (failed) {
  console.error(tag
    ? `manifests do not match tag ${tag}`
    : 'manifests disagree: run `npx tree-sitter version <version>`');
  process.exit(1);
}
console.log(`all ${Object.keys(found).length} manifests at ${expected}`);
