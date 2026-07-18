'use strict';
const fs = require('fs');
const path = require('path');
const ohm = require('ohm-js');

const AST_VERSION = '0.1';

const grammarSource = fs.readFileSync(path.join(__dirname, 'ns.ohm'), 'utf8');
const grammar = ohm.grammar(grammarSource);

// Normalize an optional Temps node into { names, inits }.
function tempsOf(tempsNode) {
  return tempsNode.numChildren ? tempsNode.children[0].toAST() : { names: [], inits: [] };
}

function span(node) {
  const iv = node.source;
  const lc = iv.getLineAndColumn();
  return {
    startOffset: iv.startIdx,
    endOffset: iv.endIdx,
    startLine: lc.lineNum,
    startCol: lc.colNum,
  };
}

const semantics = grammar.createSemantics().addOperation('toAST', {
  Program_module(classDefs) {
    // classDefs is now a IterationNode (ClassDef+), supports .children
    const body = classDefs.children.map((cd) => {
      const cls = cd.toAST();
      return { ...cls, type: 'module' };
    });
    return {
      type: 'program',
      astVersion: AST_VERSION,
      temps: [],
      body,   // one module node per top-level ClassDef
      span: span(this),
    };
  },
  Program_script(temps, stmts) {
    const t = tempsOf(temps);
    return {
      type: 'program',
      astVersion: AST_VERSION,
      temps: t.names,
      body: [...t.inits, ...stmts.toAST()],
      span: span(this),
    };
  },
  ClassDef(vis, _class, name, ctor, _eq, sup, body, _colonOpt, classSideOpt) {
    const b = body.toAST();   // { slots, methods, classes }
    const supNode = sup.numChildren ? sup.children[0].toAST() : null;
    // Disambiguate the superclass clause:
    //  - `super`/`self`/`outer` head => a navigating expression resolved by the
    //    compiler to a (possibly cross-container) delegate; exposed as superExpr.
    //    Its initializer (if any) is the trailing keyword message.
    //  - plain class-name head => the same-scope static case. `super` stays a
    //    bare string and a lone trailing unary is the parent initializer
    //    (`Base new`), preserving the existing delegate/superInit path.
    let superStr = null;
    let superExpr = null;
    let superInit = supNode ? supNode.init : null;
    if (supNode) {
      const toks = supNode.tokens;
      if (['super', 'self', 'outer'].includes(toks[0])) {
        superExpr = toks;
      } else {
        superStr = toks[0];
        if (!superInit && toks.length > 1) {
          superInit = { selector: toks[1], args: [] };
        }
      }
    }
    // Class-side ("metaclass") section after `: ( ... )`. Holds methods sent
    // to the class object itself (e.g. SOMns factory shortcuts like
    // `TaskState waitingWithPacket = ( ^ self new waitingWithPacket )`).
    const cs = classSideOpt.numChildren ? classSideOpt.children[0].toAST() : null;
    return {
      type: 'classDecl',
      name: name.sourceString,
      visibility: vis.numChildren ? vis.children[0].sourceString : null,
      params: ctor.toAST(),
      super: superStr,                              // bare superclass name (string) | null
      superExpr,                                    // [token, ...] navigating chain | null
      superInit,                                    // { selector, args } | null
      slots: b.slots,
      initBody: b.initBody,
      methods: b.methods,
      classes: b.classes,
      classMethods: cs ? cs.methods : [],          // class-side methods
      span: span(this),
    };
  },
  ClassBody(_lp, slots, stmts, _rp, _lp2, members, _rp2) {
    const methods = [];
    const classes = [];
    for (const m of members.children) {
      const ast = m.toAST();
      (ast.type === 'classDecl' ? classes : methods).push(ast);
    }
    return {
      slots: slots.numChildren ? slots.children[0].toAST() : [],
      // Executable statements in the primary-factory body (after the slot
      // section), run when an instance is created. Empty array when absent.
      initBody: stmts.toAST(),
      methods,
      classes,
    };
  },
  ClassSide_nested(body) { return body.toAST(); },
  ClassSide_flat(_lp, members, _rp) {
    const methods = [];
    const classes = [];
    for (const m of members.children) {
      const ast = m.toAST();
      (ast.type === 'classDecl' ? classes : methods).push(ast);
    }
    return { slots: [], methods, classes };
  },
  SuperClause(id, unaries, initOpt) {
    return {
      tokens: [id.sourceString, ...unaries.children.map((u) => u.sourceString)],
      init: initOpt.numChildren ? initOpt.children[0].toAST() : null,
    };
  },
  SuperInit_kw(kws, args) {
    return {
      selector: kws.children.map((k) => k.sourceString).join(''),
      args: args.children.map((a) => a.toAST()),
    };
  },
  Member(child) { return child.toAST(); },
  CtorSig_kw(pairs) { return pairs.children.map((p) => p.toAST()); },
  CtorSig_un(_sel) { return []; },
  KwPair(_kw, id) { return id.sourceString; },
  SlotSection(_open, decls, _close) {
    return decls.children.map((d) => d.toAST());
  },
  SlotDecl(vis, id, init, _dot) {
    const initAst = init.numChildren ? init.children[0].toAST() : null;
    return {
      type: 'slot',
      name: id.sourceString,
      mutable: initAst ? initAst.mutable : true,
      init: initAst ? initAst.value : null,
      visibility: vis.numChildren ? vis.children[0].sourceString : null,
      span: span(this),
    };
  },
  SlotInit(op, expr) {
    return { mutable: op.sourceString === '::=', value: expr.toAST() };
  },
  MethodDef(vis, pattern, _eq, _lp, temps, stmts, _rp) {
    const sig = pattern.toAST();
    const t = tempsOf(temps);
    return {
      type: 'method',
      selector: sig.selector,
      params: sig.params,
      visibility: vis.numChildren ? vis.children[0].sourceString : null,
      temps: t.names,
      body: [...t.inits, ...stmts.toAST()],
      span: span(this),
    };
  },
  MethodPattern_kw(pairs) {
    let selector = '';
    const params = [];
    for (const kp of pairs.children) {
      selector += kp.child(0).sourceString;   // keyword includes its trailing ':'
      params.push(kp.child(1).sourceString);
    }
    return { selector, params };
  },
  MethodPattern_bin(op, id) {
    return { selector: op.sourceString, params: [id.sourceString] };
  },
  MethodPattern_un(sel) {
    return { selector: sel.sourceString, params: [] };
  },
  Temps(_open, decls, _close) {
    // Returns { names, inits }. Initialized temps (`| s = expr. t |`) are
    // lowered to a plain name plus a synthesized assignment that consumers
    // prepend to the body, so the emitted AST keeps `temps` as a name list.
    const names = [];
    const inits = [];
    for (const d of decls.children) {
      const t = d.toAST();
      names.push(t.name);
      if (t.init) {
        inits.push({ type: 'assign', target: t.name, value: t.init, span: t.span });
      }
    }
    return { names, inits };
  },
  TempDecl(id, init, _dot) {
    const initAst = init.numChildren ? init.children[0].toAST() : null;
    return { name: id.sourceString, init: initAst ? initAst.value : null, span: span(this) };
  },
  Statements(list, _trailingDot) {
    return list.asIteration().children.map((c) => c.toAST());
  },
  Statement(s) { return s.toAST(); },
  Return(_caret, value) { return { type: 'return', value: value.toAST(), span: span(this) }; },
  integer(_neg, _d) {
    // Keep the raw decimal text: JS numbers are float64 and would silently lose
    // precision above 2^53 (the compiler parses it exactly with Python int()).
    return { type: 'literal', litKind: 'int', value: this.sourceString, span: span(this) };
  },
  float(_neg, _a, _dot, _b) {
    return { type: 'literal', litKind: 'float', value: parseFloat(this.sourceString), span: span(this) };
  },
  string(_open, _chars, _close) {
    const raw = this.sourceString.slice(1, -1).replace(/''/g, "'");
    return { type: 'literal', litKind: 'string', value: raw, span: span(this) };
  },
  symbol(_hash, name) {
    return { type: 'literal', litKind: 'symbol', value: name.sourceString, span: span(this) };
  },
  Literal_true(_t) {
    return { type: 'literal', litKind: 'bool', value: true, span: span(this) };
  },
  Literal_false(_f) {
    return { type: 'literal', litKind: 'bool', value: false, span: span(this) };
  },
  Literal_nil(_n) {
    return { type: 'literal', litKind: 'nil', value: null, span: span(this) };
  },
  Assignment_assign(id, _op, value) {
    return { type: 'assign', target: id.sourceString, value: value.toAST(), span: span(this) };
  },
  Assignment_pass(e) { return e.toAST(); },
  KeywordSend_kw(recv, kws, args) {
    const selector = kws.children.map((k) => k.sourceString).join('');
    return {
      type: 'send',
      receiver: recv.toAST(),
      selector,
      args: args.children.map((a) => a.toAST()),
      span: span(this),
    };
  },
  KeywordSend_implicitKw(kws, args) {
    const selector = kws.children.map((k) => k.sourceString).join('');
    return {
      type: 'send',
      receiver: { type: 'implicitReceiver', span: span(this) },
      selector,
      args: args.children.map((a) => a.toAST()),
      span: span(this),
    };
  },
  KeywordSend_pass(e) { return e.toAST(); },
  BinarySend_bin(recv, op, arg) {
    return { type: 'send', receiver: recv.toAST(), selector: op.sourceString, args: [arg.toAST()], span: span(this) };
  },
  BinarySend_pass(e) { return e.toAST(); },
  UnarySend_un(recv, msg) {
    return { type: 'send', receiver: recv.toAST(), selector: msg.sourceString, args: [], span: span(this) };
  },
  UnarySend_pass(e) { return e.toAST(); },
  Primary_paren(_l, e, _r) { return e.toAST(); },
  Primary_var(id) { return { type: 'varRef', name: id.sourceString, span: span(this) }; },
  DynArray(_lb, items, _dot, _rb) {
    return {
      type: 'dynArray',
      elements: items.asIteration().children.map((e) => e.toAST()),
      span: span(this),
    };
  },
  Block(_l, params, temps, body, _r) {
    const t = tempsOf(temps);
    return {
      type: 'block',
      params: params.numChildren ? params.children[0].toAST() : [],
      temps: t.names,
      body: [...t.inits, ...body.toAST()],
      span: span(this),
    };
  },
  BlockParams(ps, _bar) {
    return ps.children.map((p) => p.toAST());
  },
  blockParam(_colon, id) {
    return id.sourceString;
  },
});

function parse(source) {
  const m = grammar.match(source);
  if (m.failed()) {
    const err = new Error(m.message);
    err.stage = 'parse';
    err.shortMessage = m.shortMessage;
    throw err;
  }
  return semantics(m).toAST();
}

module.exports = { parse, grammar, AST_VERSION };

if (require.main === module) {
  const file = process.argv[2];
  if (!file) {
    process.stderr.write('usage: node ns-parse.js <file.nsl>\n');
    process.exit(2);
  }
  try {
    const source = fs.readFileSync(file, 'utf8');
    process.stdout.write(JSON.stringify(parse(source)));
  } catch (e) {
    const stage = e.stage ?? (e.code ? 'io' : 'parse');
    process.stderr.write(JSON.stringify({ error: String(e.message || e), stage }));
    process.exit(1);
  }
}
