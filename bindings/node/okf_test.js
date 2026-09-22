const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const { test } = require('node:test');

const { evaluate } = require('../../script/helpers-fixtures');

const cases = JSON.parse(
  fs.readFileSync(path.join(__dirname, '..', '..', 'test', 'helpers', 'cases.json'), 'utf8'),
).cases;

for (const c of cases) {
  test(`okf helpers: ${c.name}`, () => {
    assert.deepStrictEqual(evaluate(c), c.expect);
  });
}
