'use strict';
const { test } = require('node:test');
const assert = require('node:assert');
const { parse } = require('../ns-parse.js');

const expr = (src) => parse(src).body[0];

test('unary send', () => {
  const n = expr('3 println');
  assert.strictEqual(n.type, 'send');
  assert.strictEqual(n.selector, 'println');
  assert.deepStrictEqual(n.args, []);
  assert.strictEqual(n.receiver.value, 3);
});

test('binary send', () => {
  const n = expr('3 + 4');
  assert.strictEqual(n.type, 'send');
  assert.strictEqual(n.selector, '+');
  assert.strictEqual(n.receiver.value, 3);
  assert.strictEqual(n.args[0].value, 4);
});

test('unary binds tighter than binary', () => {
  // 3 factorial + 4  ==  (3 factorial) + 4
  const n = expr('3 factorial + 4');
  assert.strictEqual(n.selector, '+');
  assert.strictEqual(n.receiver.type, 'send');
  assert.strictEqual(n.receiver.selector, 'factorial');
  assert.strictEqual(n.args[0].value, 4);
});

test('binary is left-associative', () => {
  // 2 + 3 + 4  ==  (2 + 3) + 4
  const n = expr('2 + 3 + 4');
  assert.strictEqual(n.selector, '+');
  assert.strictEqual(n.args[0].value, 4);
  assert.strictEqual(n.receiver.selector, '+');
  assert.strictEqual(n.receiver.receiver.value, 2);
});

test('keyword send is looser than binary; selector is concatenated', () => {
  // a at: 1 + 2 put: 9  ==  a at:(1+2) put: 9
  const n = expr('a at: 1 + 2 put: 9');
  assert.strictEqual(n.type, 'send');
  assert.strictEqual(n.selector, 'at:put:');
  assert.strictEqual(n.receiver.type, 'varRef');
  assert.strictEqual(n.receiver.name, 'a');
  assert.strictEqual(n.args.length, 2);
  assert.strictEqual(n.args[0].selector, '+'); // 1 + 2
  assert.strictEqual(n.args[1].value, 9);
});

test('implicit-receiver keyword send', () => {
  // `foo: true` with no explicit receiver is a self/implicit send
  const n = expr('oneLevelInliningWithLocalShadow: true');
  assert.strictEqual(n.type, 'send');
  assert.strictEqual(n.selector, 'oneLevelInliningWithLocalShadow:');
  assert.strictEqual(n.receiver.type, 'implicitReceiver');
  assert.strictEqual(n.args.length, 1);
  assert.strictEqual(n.args[0].value, true);
});

test('implicit-receiver multi-keyword send', () => {
  const n = expr('at: 1 put: 2');
  assert.strictEqual(n.selector, 'at:put:');
  assert.strictEqual(n.receiver.type, 'implicitReceiver');
  assert.strictEqual(n.args.length, 2);
});

test('explicit keyword send still has its receiver', () => {
  const n = expr('a foo: 1');
  assert.strictEqual(n.selector, 'foo:');
  assert.strictEqual(n.receiver.type, 'varRef');
  assert.strictEqual(n.receiver.name, 'a');
});

test('unary is left-associative', () => {
  // 3 factorial factorial == (3 factorial) factorial
  const n = expr('3 factorial factorial');
  assert.strictEqual(n.selector, 'factorial');
  assert.strictEqual(n.receiver.type, 'send');
  assert.strictEqual(n.receiver.selector, 'factorial');
  assert.strictEqual(n.receiver.receiver.value, 3);
});

test('parentheses override precedence', () => {
  // (2 + 3) factorial : the unary applies to the whole parenthesised binary
  const n = expr('(2 + 3) factorial');
  assert.strictEqual(n.selector, 'factorial');
  assert.strictEqual(n.receiver.type, 'send');
  assert.strictEqual(n.receiver.selector, '+');
  assert.strictEqual(n.receiver.receiver.value, 2);
  assert.strictEqual(n.receiver.args[0].value, 3);
});
