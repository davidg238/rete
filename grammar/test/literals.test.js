'use strict';
const { test } = require('node:test');
const assert = require('node:assert');
const { parse } = require('../ns-parse.js');

function firstStatement(src) {
  const ast = parse(src);
  assert.strictEqual(ast.type, 'program');
  assert.strictEqual(ast.astVersion, '0.1');
  return ast.body[0];
}

test('integer literal', () => {
  const n = firstStatement('42');
  assert.strictEqual(n.type, 'literal');
  assert.strictEqual(n.litKind, 'int');
  assert.strictEqual(n.value, 42);
});

test('float literal', () => {
  const n = firstStatement('3.5');
  assert.strictEqual(n.litKind, 'float');
  assert.strictEqual(n.value, 3.5);
});

test('negative integer literal', () => {
  const n = firstStatement('-42');
  assert.strictEqual(n.litKind, 'int');
  assert.strictEqual(n.value, -42);
});

test('negative float literal', () => {
  const n = firstStatement('-0.169');
  assert.strictEqual(n.litKind, 'float');
  assert.strictEqual(n.value, -0.169);
});

test('min 64-bit integer literal', () => {
  const n = firstStatement('-9223372036854775808');
  assert.strictEqual(n.litKind, 'int');
});

test('binary minus still parses as subtraction, not negative literal', () => {
  const n = firstStatement('a - 1');
  assert.strictEqual(n.type, 'send');
  assert.strictEqual(n.selector, '-');
  assert.strictEqual(n.receiver.type, 'varRef');
  assert.strictEqual(n.args[0].value, 1);
});

test('subtraction without spaces parses as subtraction', () => {
  const n = firstStatement('3-4');
  assert.strictEqual(n.type, 'send');
  assert.strictEqual(n.selector, '-');
  assert.strictEqual(n.receiver.value, 3);
  assert.strictEqual(n.args[0].value, 4);
});

test('negated argument: 3 - -4', () => {
  const n = firstStatement('3 - -4');
  assert.strictEqual(n.selector, '-');
  assert.strictEqual(n.args[0].value, -4);
});

test('string literal with doubled-quote escape', () => {
  const n = firstStatement("'it''s ok'");
  assert.strictEqual(n.litKind, 'string');
  assert.strictEqual(n.value, "it's ok");
});

test('boolean and nil literals', () => {
  assert.deepStrictEqual(
    ['true', 'false', 'nil'].map((s) => firstStatement(s).value),
    [true, false, null]
  );
});

test('symbol literal', () => {
  const n = firstStatement('#foo');
  assert.strictEqual(n.litKind, 'symbol');
  assert.strictEqual(n.value, 'foo');
});

test('keyword symbol literal', () => {
  const n = firstStatement('#at:put:');
  assert.strictEqual(n.litKind, 'symbol');
  assert.strictEqual(n.value, 'at:put:');
});

test('binary-operator symbol literal', () => {
  const n = firstStatement('#+');
  assert.strictEqual(n.litKind, 'symbol');
  assert.strictEqual(n.value, '+');
});

test('span carries exact 1-based line/col and 0-based offsets', () => {
  const n = firstStatement('42');
  assert.strictEqual(n.span.startOffset, 0);
  assert.strictEqual(n.span.endOffset, 2);
  assert.strictEqual(n.span.startLine, 1);
  assert.strictEqual(n.span.startCol, 1);
});
