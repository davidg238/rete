'use strict';
const { test } = require('node:test');
const assert = require('node:assert');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { parse } = require('../ns-parse.js');

test('multi-statement program with trailing period', () => {
  const ast = parse('3 + 4. 5 println.');
  assert.strictEqual(ast.body.length, 2);
  assert.strictEqual(ast.body[0].selector, '+');
  assert.strictEqual(ast.body[1].selector, 'println');
});

test('block with params parses (lowering deferred to M1)', () => {
  const n = parse('[ :x | x + 1 ]').body[0];
  assert.strictEqual(n.type, 'block');
  assert.deepStrictEqual(n.params, ['x']);
  assert.strictEqual(n.body[0].selector, '+');
});

test('zero-arg block parses', () => {
  const n = parse('[ 42 ]').body[0];
  assert.strictEqual(n.type, 'block');
  assert.deepStrictEqual(n.params, []);
  assert.strictEqual(n.body[0].value, 42);
});

test('comment is ignored', () => {
  const ast = parse('(* a note *) 7');
  assert.strictEqual(ast.body[0].value, 7);
});

test('CLI prints JSON AST for a .nsl file', () => {
  const f = path.join(os.tmpdir(), `ns_cli_${process.pid}.nsl`);
  fs.writeFileSync(f, '(3 + 4) println');
  try {
    const out = execFileSync('node', [path.join(__dirname, '..', 'ns-parse.js'), f], { encoding: 'utf8' });
    const ast = JSON.parse(out);
    assert.strictEqual(ast.type, 'program');
    assert.strictEqual(ast.body[0].selector, 'println');
  } finally {
    fs.unlinkSync(f);
  }
});

test('CLI reports a parse error as JSON on stderr with non-zero exit', () => {
  const f = path.join(os.tmpdir(), `ns_bad_${process.pid}.nsl`);
  fs.writeFileSync(f, '3 +');
  try {
    let threw = false;
    try {
      execFileSync('node', [path.join(__dirname, '..', 'ns-parse.js'), f], { encoding: 'utf8', stdio: 'pipe' });
    } catch (e) {
      threw = true;
      const payload = JSON.parse(e.stderr);
      assert.strictEqual(payload.stage, 'parse');
    }
    assert.ok(threw, 'CLI should exit non-zero on parse failure');
  } finally {
    fs.unlinkSync(f);
  }
});

test('CLI reports a missing file as stage io, not parse', () => {
  const missing = path.join(os.tmpdir(), `ns_nope_${process.pid}_does_not_exist.nsl`);
  let threw = false;
  try {
    execFileSync('node', [path.join(__dirname, '..', 'ns-parse.js'), missing], { encoding: 'utf8', stdio: 'pipe' });
  } catch (e) {
    threw = true;
    const payload = JSON.parse(e.stderr);
    assert.strictEqual(payload.stage, 'io');
  }
  assert.ok(threw, 'CLI should exit non-zero when the file is missing');
});
