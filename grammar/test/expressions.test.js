'use strict';
const { test } = require('node:test');
const assert = require('node:assert');
const { parse } = require('../ns-parse.js');

test('program with no temporaries has an empty temps list', () => {
  const ast = parse('42');
  assert.strictEqual(ast.type, 'program');
  assert.deepStrictEqual(ast.temps, []);
  assert.strictEqual(ast.body[0].value, 42);
});

test('leading temporaries are captured on the program', () => {
  const ast = parse('| a b | a');
  assert.deepStrictEqual(ast.temps, ['a', 'b']);
  assert.strictEqual(ast.body.length, 1);
  assert.strictEqual(ast.body[0].type, 'varRef');
  assert.strictEqual(ast.body[0].name, 'a');
});

test('empty temporaries pipe parses', () => {
  const ast = parse('| | 7');
  assert.deepStrictEqual(ast.temps, []);
  assert.strictEqual(ast.body[0].value, 7);
});

test('block-local temporaries are captured on the block', () => {
  const n = parse('[ | t | t ]').body[0];
  assert.strictEqual(n.type, 'block');
  assert.deepStrictEqual(n.params, []);
  assert.deepStrictEqual(n.temps, ['t']);
  assert.strictEqual(n.body[0].name, 't');
});

test('block with both params and temporaries', () => {
  const n = parse('[ :x | | t | t ]').body[0];
  assert.deepStrictEqual(n.params, ['x']);
  assert.deepStrictEqual(n.temps, ['t']);
});

test('bar still parses as binary-or when not in temp position', () => {
  const n = parse('a | b').body[0];
  assert.strictEqual(n.type, 'send');
  assert.strictEqual(n.selector, '|');
});

test('simple assignment', () => {
  const n = parse('x:: 3').body[0];
  assert.strictEqual(n.type, 'assign');
  assert.strictEqual(n.target, 'x');
  assert.strictEqual(n.value.type, 'literal');
  assert.strictEqual(n.value.value, 3);
});

test('assignment value can be a full expression', () => {
  const n = parse('x:: 1 + 2').body[0];
  assert.strictEqual(n.type, 'assign');
  assert.strictEqual(n.target, 'x');
  assert.strictEqual(n.value.type, 'send');
  assert.strictEqual(n.value.selector, '+');
});

test('assignment is right-associative', () => {
  // x:: y:: 5  ==  x:: (y:: 5)
  const n = parse('x:: y:: 5').body[0];
  assert.strictEqual(n.type, 'assign');
  assert.strictEqual(n.target, 'x');
  assert.strictEqual(n.value.type, 'assign');
  assert.strictEqual(n.value.target, 'y');
  assert.strictEqual(n.value.value.value, 5);
});

test('keyword send is not mistaken for assignment', () => {
  const n = parse('a foo: 1').body[0];
  assert.strictEqual(n.type, 'send');
  assert.strictEqual(n.selector, 'foo:');
});

test('return statement', () => {
  const n = parse('^ 42').body[0];
  assert.strictEqual(n.type, 'return');
  assert.strictEqual(n.value.type, 'literal');
  assert.strictEqual(n.value.value, 42);
});

test('return of an expression', () => {
  const n = parse('^ 1 + 2').body[0];
  assert.strictEqual(n.type, 'return');
  assert.strictEqual(n.value.type, 'send');
  assert.strictEqual(n.value.selector, '+');
});

test('return inside a block parses (lowering decides inlined vs reified)', () => {
  const blk = parse('[ ^ 5 ]').body[0];
  assert.strictEqual(blk.type, 'block');
  assert.strictEqual(blk.body[0].type, 'return');
  assert.strictEqual(blk.body[0].value.value, 5);
});

test('statements mix returns and expressions', () => {
  const ast = parse('x:: 1. ^ x');
  assert.strictEqual(ast.body.length, 2);
  assert.strictEqual(ast.body[0].type, 'assign');
  assert.strictEqual(ast.body[1].type, 'return');
});
