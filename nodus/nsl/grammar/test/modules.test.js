'use strict';
const { test } = require('node:test');
const assert = require('node:assert');
const { parse } = require('../ns-parse.js');

test('a script file still parses as a program (no module)', () => {
  const ast = parse('| a | a:: 1. ^ a');
  assert.strictEqual(ast.type, 'program');
  assert.strictEqual(ast.body[0].type, 'assign');
});

test('nullary module: empty slots and methods', () => {
  const ast = parse('class Counter = ( )( )');
  assert.strictEqual(ast.type, 'program');
  assert.strictEqual(ast.body.length, 1);
  const m = ast.body[0];
  assert.strictEqual(m.type, 'module');
  assert.strictEqual(m.name, 'Counter');
  assert.deepStrictEqual(m.params, []);
  assert.deepStrictEqual(m.slots, []);
  assert.deepStrictEqual(m.methods, []);
});

test('usingPlatform: constructor captures the capability param name', () => {
  const m = parse('class SensorNode usingPlatform: platform = ( )( )').body[0];
  assert.strictEqual(m.name, 'SensorNode');
  assert.deepStrictEqual(m.params, ['platform']);
});

test('multi-keyword constructor captures every param', () => {
  const m = parse('class Dual usingPlatform: p ports: q = ( )( )').body[0];
  assert.deepStrictEqual(m.params, ['p', 'q']);
});

test('a "class"-prefixed identifier parses as a script, not a module', () => {
  const ast = parse('classy bar');
  assert.strictEqual(ast.type, 'program');
  assert.strictEqual(ast.body[0].type, 'send');
  assert.strictEqual(ast.body[0].selector, 'bar');
});

test('an empty block parses (no longer crashes the parser)', () => {
  const blk = parse('[]').body[0];
  assert.strictEqual(blk.type, 'block');
  assert.deepStrictEqual(blk.body, []);
});

test('immutable slot with initializer', () => {
  const m = parse('class C = ( | x = 1. | )( )').body[0];
  assert.strictEqual(m.slots.length, 1);
  const s = m.slots[0];
  assert.strictEqual(s.type, 'slot');
  assert.strictEqual(s.name, 'x');
  assert.strictEqual(s.mutable, false);
  assert.strictEqual(s.init.type, 'literal');
  assert.strictEqual(s.init.value, 1);
  assert.strictEqual(s.visibility, null);
});

test('mutable slot with initializer', () => {
  const s = parse('class C = ( | count ::= 0. | )( )').body[0].slots[0];
  assert.strictEqual(s.name, 'count');
  assert.strictEqual(s.mutable, true);
  assert.strictEqual(s.init.value, 0);
});

test('bare slot (no initializer) is mutable with null init', () => {
  const s = parse('class C = ( | total. | )( )').body[0].slots[0];
  assert.strictEqual(s.name, 'total');
  assert.strictEqual(s.mutable, true);
  assert.strictEqual(s.init, null);
});

test('slot initializer can be a full expression', () => {
  const s = parse('class C usingPlatform: p = ( | clk = p clock. | )( )').body[0].slots[0];
  assert.strictEqual(s.name, 'clk');
  assert.strictEqual(s.init.type, 'send');
  assert.strictEqual(s.init.selector, 'clock');
});

test('visibility keyword is captured but does not become the slot name', () => {
  const s = parse('class C = ( | private secret = 1. | )( )').body[0].slots[0];
  assert.strictEqual(s.visibility, 'private');
  assert.strictEqual(s.name, 'secret');
});

test('multiple slots', () => {
  const slots = parse('class C = ( | a = 1. b ::= 2. c. | )( )').body[0].slots;
  assert.deepStrictEqual(slots.map((s) => s.name), ['a', 'b', 'c']);
  assert.deepStrictEqual(slots.map((s) => s.mutable), [false, true, true]);
});

test('unary method', () => {
  const m = parse('class C = ( )( bump = ( 1 + 1 ) )').body[0];
  assert.strictEqual(m.methods.length, 1);
  const meth = m.methods[0];
  assert.strictEqual(meth.type, 'method');
  assert.strictEqual(meth.selector, 'bump');
  assert.deepStrictEqual(meth.params, []);
  assert.strictEqual(meth.body[0].type, 'send');
});

test('keyword method captures colon-joined selector and params', () => {
  const meth = parse('class C = ( )( at: i put: v = ( v ) )').body[0].methods[0];
  assert.strictEqual(meth.selector, 'at:put:');
  assert.deepStrictEqual(meth.params, ['i', 'v']);
});

test('binary method', () => {
  const meth = parse('class C = ( )( + other = ( other ) )').body[0].methods[0];
  assert.strictEqual(meth.selector, '+');
  assert.deepStrictEqual(meth.params, ['other']);
});

test('method with temporaries and multiple statements', () => {
  const meth = parse('class C = ( )( run = ( | t | t:: 1. ^ t ) )').body[0].methods[0];
  assert.deepStrictEqual(meth.temps, ['t']);
  assert.strictEqual(meth.body.length, 2);
  assert.strictEqual(meth.body[1].type, 'return');
});

test('empty method body', () => {
  const meth = parse('class C = ( )( noop = ( ) )').body[0].methods[0];
  assert.deepStrictEqual(meth.temps, []);
  assert.deepStrictEqual(meth.body, []);
});

test('several methods', () => {
  const m = parse('class C = ( | n ::= 0. | )( bump = ( n:: n + 1 ) value = ( n ) )').body[0];
  assert.deepStrictEqual(m.methods.map((x) => x.selector), ['bump', 'value']);
});

test('self parses as a varRef (lowering special-cases it)', () => {
  const meth = parse('class C = ( )( go = ( self bump ) )').body[0].methods[0];
  const recv = meth.body[0].receiver;
  assert.strictEqual(recv.type, 'varRef');
  assert.strictEqual(recv.name, 'self');
});

test('existing flat module gains empty classes[] and null super', () => {
  const m = parse('class C usingPlatform: p = ( | x ::= 0. | )( go = ( x ) )').body[0];
  assert.deepStrictEqual(m.classes, []);
  assert.strictEqual(m.super, null);
});

test('a nested class is captured under classes[], not methods[]', () => {
  const m = parse('class App = ( )( class Inner = ( )( ping = ( 1 ) ) main = ( 1 ) )').body[0];
  assert.deepStrictEqual(m.methods.map((x) => x.selector), ['main']);
  assert.strictEqual(m.classes.length, 1);
  const inner = m.classes[0];
  assert.strictEqual(inner.type, 'classDecl');
  assert.strictEqual(inner.name, 'Inner');
  assert.deepStrictEqual(inner.params, []);
  assert.deepStrictEqual(inner.slots, []);
  assert.deepStrictEqual(inner.classes, []);
  assert.strictEqual(inner.super, null);
  assert.deepStrictEqual(inner.methods.map((x) => x.selector), ['ping']);
});

test('a nested class keeps its own ctor params and slots', () => {
  const inner = parse(
    'class App = ( )( class Drv usingPlatform: q = ( | n ::= 0. | )( tick = ( n ) ) )'
  ).body[0].classes[0];
  assert.deepStrictEqual(inner.params, ['q']);
  assert.deepStrictEqual(inner.slots.map((s) => s.name), ['n']);
});

test('a superclass clause is captured as the class name string', () => {
  const m = parse('class App = ( )( class Animal = ( )( ) class Dog = Animal ( )( ) )').body[0];
  const dog = m.classes.find((c) => c.name === 'Dog');
  assert.strictEqual(dog.super, 'Animal');
});

test('the top module can itself carry a superclass clause', () => {
  const m = parse('class App = Base ( )( )').body[0];
  assert.strictEqual(m.super, 'Base');
});

test('methods and nested classes can interleave in the member section', () => {
  const m = parse('class App = ( )( a = ( 1 ) class K = ( )( ) b = ( 2 ) )').body[0];
  assert.deepStrictEqual(m.methods.map((x) => x.selector), ['a', 'b']);
  assert.deepStrictEqual(m.classes.map((x) => x.name), ['K']);
});

test('an empty member section yields empty classes[] and methods[]', () => {
  const m = parse('class C = ( )( )').body[0];
  assert.deepStrictEqual(m.classes, []);
  assert.deepStrictEqual(m.methods, []);
});

test('deeper-than-one-level nesting parses (rejected later in ns_lower, not the grammar)', () => {
  const m = parse('class A = ( )( class B = ( )( class C = ( )( ) ) )').body[0];
  assert.strictEqual(m.classes[0].name, 'B');
  assert.strictEqual(m.classes[0].classes[0].name, 'C');
});

test('a nested class can carry super, ctor params, and slots together', () => {
  const inner = parse(
    'class App = ( )( class Animal = ( )( ) class Dog newWith: x = Animal ( | n ::= 0. | )( go = ( n ) ) )'
  ).body[0].classes.find((c) => c.name === 'Dog');
  assert.strictEqual(inner.super, 'Animal');
  assert.deepStrictEqual(inner.params, ['x']);
  assert.deepStrictEqual(inner.slots.map((s) => s.name), ['n']);
});
