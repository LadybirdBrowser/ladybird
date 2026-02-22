// This test suite verifies that malformed JavaScript inputs produce graceful
// SyntaxError results instead of crashing the process via panics/aborts in
// the parser, scope collector, or code generator.

test("syntax errors inside functions don't crash scope collector", () => {
    expect("function f() { let x = ; }").not.toEval();
    expect("function f() { let x = }").not.toEval();
    expect("function f() { return }; function g() { let = }").not.toEval();
    expect("function f(a, b, ...c) { let x = ; }").not.toEval();
    expect("function f() { { { { let x = ; } } } }").not.toEval();
    expect("function f() { var x = ; }").not.toEval();
    expect("function f() { const x }").not.toEval();
    expect("function f() { const }").not.toEval();
});

test("syntax errors inside blocks don't crash scope collector", () => {
    expect("{ let x = ; }").not.toEval();
    expect("{ const x = ; }").not.toEval();
    expect("{ let x = 1; { let y = ; } }").not.toEval();
    expect("{ var x = ; }").not.toEval();
});

test("syntax errors inside try-catch don't crash scope collector", () => {
    expect("try { let x = ; } catch (e) {}").not.toEval();
    expect("try {} catch (e) { let x = ; }").not.toEval();
    expect("try {} catch { let x = ; }").not.toEval();
    expect("try {} finally { let x = ; }").not.toEval();
    expect("try { class C { } catch (e) {}").not.toEval();
    expect("try { } catch (").not.toEval();
    expect("try { } catch (e").not.toEval();
    expect("try {").not.toEval();
});

test("syntax errors inside with statements don't crash scope collector", () => {
    expect("with (x) { let y = ; }").not.toEval();
    expect("with (x) {").not.toEval();
    expect("with (x").not.toEval();
    expect("with (").not.toEval();
});

test("syntax errors inside for loops don't crash scope collector", () => {
    expect("for (let x = 0; x < ; x++) {}").not.toEval();
    expect("for (let x = ; ;) {}").not.toEval();
    expect("for (let x").not.toEval();
    expect("for (let x of").not.toEval();
    expect("for (let x in").not.toEval();
    expect("for (var x = ; ;) {}").not.toEval();
    expect("for (const x of [1]) { let y = ; }").not.toEval();
    expect("for (let x = 0; x < 10; x++) { let y = ; }").not.toEval();
});

test("syntax errors inside classes don't crash scope collector", () => {
    expect("class C { constructor() { let x = ; } }").not.toEval();
    expect("class C { method() { let x = ; } }").not.toEval();
    expect("class C { static { let x = ; } }").not.toEval();
    expect("class C { static x = ; }").not.toEval();
    expect("class C extends").not.toEval();
    expect("class C {").not.toEval();
    expect("class {").not.toEval();
    expect("class C { #x = ; }").not.toEval();
    expect("class C { get x() { let y = ; } }").not.toEval();
    expect("class C { set x(v) { let y = ; } }").not.toEval();
});

test("syntax errors inside switch don't crash scope collector", () => {
    expect("switch (x) { case 1: let y = ; }").not.toEval();
    expect("switch (x) { default: let y = ; }").not.toEval();
    expect("switch (x) {").not.toEval();
    expect("switch (").not.toEval();
});

test("partial variable declarations don't crash analyze()", () => {
    expect("let x =").not.toEval();
    expect("var").not.toEval();
    expect("const x").not.toEval();
    // NB: "let" alone is a valid expression (identifier) in non-strict mode.
    expect("'use strict'; let").not.toEval();
    expect("let x, y =").not.toEval();
    expect("var x =").not.toEval();
    expect("const x =").not.toEval();
    expect("let [x] =").not.toEval();
    expect("let {x} =").not.toEval();
    expect("var x, y, z =").not.toEval();
});

test("incomplete function parameters don't crash", () => {
    expect("function f(").not.toEval();
    expect("function f(a,").not.toEval();
    expect("function f(a, ...").not.toEval();
    expect("function f(a, b = ").not.toEval();
    expect("(a, b, ...c) =>").not.toEval();
    expect("function f({x} =").not.toEval();
    expect("function f([a, b] =").not.toEval();
});

test("errors after variable binding but before scope closes", () => {
    expect("function f() { let x = 1; let y = ; }").not.toEval();
    expect("function f() { var x = 1; return ; var y = ; }").not.toEval();
    expect("{ let x = 1; let y = 2; let z = ; }").not.toEval();
    expect("for (let x = 0; ;) { let y = ; }").not.toEval();
    expect("function f() { let x = 1; { let y = 2; let z = ; } }").not.toEval();
});

test("break/continue outside loops produce SyntaxError, not crash", () => {
    expect("break").not.toEval();
    expect("continue").not.toEval();
    expect("{ break }").not.toEval();
    expect("{ continue }").not.toEval();
    expect("switch (x) { case 1: continue; }").not.toEval();
});

test("return outside function produces SyntaxError, not crash", () => {
    expect("return").not.toEval();
    expect("return 1").not.toEval();
    expect("{ return }").not.toEval();
});

test("malformed class constructors don't crash", () => {
    expect("class C { constructor").not.toEval();
    expect("class C { constructor(").not.toEval();
    expect("class C { constructor() {").not.toEval();
    expect("class C extends B { constructor() { super(").not.toEval();
});

test("malformed finally blocks don't crash", () => {
    expect("try {} finally {").not.toEval();
    expect("try {} finally").not.toEval();
    expect("try {} catch {} finally {").not.toEval();
    expect("try { try {} finally { let x = ; } } catch {}").not.toEval();
});

test("nested template literals don't crash lexer", () => {
    expect("`${`${1}`}`").toEval();
    expect("`${`${`${1}`}`}`").toEval();
    expect("`${").not.toEval();
    expect("`${`").not.toEval();
    expect("`${`${").not.toEval();
    expect("`${{").not.toEval();
});

test("malformed arrow functions don't crash", () => {
    expect("() =>").not.toEval();
    expect("() => {").not.toEval();
    expect("x =>").not.toEval();
    expect("(x, y) => { let z = ; }").not.toEval();
    expect("async () =>").not.toEval();
    expect("async () => {").not.toEval();
});

test("malformed destructuring doesn't crash", () => {
    expect("let {x, y: {z}} = ;").not.toEval();
    expect("let [a, [b, c]] = ;").not.toEval();
    expect("let {x, ...} = obj").not.toEval();
    expect("let {x,,} = obj").not.toEval();
    expect("function f({x, y} = ) {}").not.toEval();
});

test("deeply nested scope errors don't crash", () => {
    expect("function a() { function b() { function c() { let x = ; } } }").not.toEval();
    expect("function a() { { { { { let x = ; } } } } }").not.toEval();
    expect("() => { () => { () => { let x = ; } } }").not.toEval();
});

test("eval with malformed input doesn't crash", () => {
    expect(() => eval("let x = ;")).toThrow(SyntaxError);
    expect(() => eval("function f() { let x = ; }")).toThrow(SyntaxError);
    expect(() => eval("{")).toThrow(SyntaxError);
    expect(() => eval("class C {")).toThrow(SyntaxError);
    expect(() => eval("for (let x")).toThrow(SyntaxError);
    expect(() => eval("try {")).toThrow(SyntaxError);
});

test("new Function with malformed body doesn't crash", () => {
    expect(() => new Function("let x = ;")).toThrow(SyntaxError);
    expect(() => new Function("a", "let x = ;")).toThrow(SyntaxError);
    expect(() => new Function("{")).toThrow(SyntaxError);
    expect(() => new Function("for (let x")).toThrow(SyntaxError);
    expect(() => new Function("try {")).toThrow(SyntaxError);
    expect(() => new Function("...")).toThrow(SyntaxError);
});

test("new Function with malformed parameters doesn't crash", () => {
    expect(() => new Function("a b", "return a")).toThrow(SyntaxError);
    expect(() => new Function("{x}", "return x")).not.toThrow();
    expect(() => new Function("/*", "*/){} return 1; function(")).toThrow(SyntaxError);
});

test("generator and async variants with malformed input don't crash", () => {
    expect("function* g() { let x = ; }").not.toEval();
    expect("function* g() { yield ; let x = ; }").not.toEval();
    expect("async function f() { let x = ; }").not.toEval();
    expect("async function f() { await ; let x = ; }").not.toEval();
    expect("async function* f() { let x = ; }").not.toEval();
});

test("labeled statement errors don't crash", () => {
    expect("label: { let x = ; }").not.toEval();
    expect("label: for (let x = ; ;) {}").not.toEval();
    expect("outer: inner: { let x = ; }").not.toEval();
});

test("object and array literals with errors don't crash", () => {
    expect("({x: })").not.toEval();
    expect("({...})").not.toEval();
    expect("({get x() { let y = ; }})").not.toEval();
    expect("({set x(v) { let y = ; }})").not.toEval();
    expect("[...").not.toEval();
    expect("[,,,").not.toEval();
});
