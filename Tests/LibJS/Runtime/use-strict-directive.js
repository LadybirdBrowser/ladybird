function isStrictFunction(body) {
    return new Function("info", "instanceofs", `${body}\n;return this === undefined;`)();
}

function isStrictScript(source) {
    return (0, eval)(`${source}\n;(function () { return this === undefined; })();`);
}

test("a use strict directive that ends where a statement starts on the next line", () => {
    expect(isStrictFunction('"use strict"\n++globalThis.n')).toBeTrue();
    expect(isStrictFunction('"use strict"\n--globalThis.n')).toBeTrue();
    expect(isStrictFunction('"use strict"\n.5.toFixed()')).toBeTrue();
    expect(isStrictFunction('"use strict"\ninfo')).toBeTrue();
    expect(isStrictFunction('"use strict"\ninstanceofs')).toBeTrue();
    expect(isStrictScript('"use strict"\n++globalThis.n')).toBeTrue();
});

test("a string that an operator on the next line continues is no directive", () => {
    expect(isStrictFunction('"use strict"\nin {}')).toBeFalse();
    expect(isStrictFunction('"use strict"\ninstanceof Object')).toBeFalse();
    expect(isStrictFunction('"use strict"\n+ 1')).toBeFalse();
    expect(isStrictFunction('"use strict"\n- 1')).toBeFalse();
    expect(isStrictFunction('"use strict"\n.length')).toBeFalse();
    expect(isStrictFunction('"use strict"\n!= 1')).toBeFalse();
    expect(isStrictScript('"use strict"\nin {}')).toBeFalse();
    expect(isStrictFunction('"use strict"\nin {}; with ({}) {}')).toBeFalse();
});

test("HTML-like comments around a use strict directive", () => {
    expect(isStrictFunction('"use strict"\n<!-- comment')).toBeTrue();
    expect(isStrictFunction('"use strict"\n--> comment')).toBeTrue();
    expect(isStrictFunction('"use strict" <!-- comment')).toBeTrue();
    expect(isStrictFunction('\n<!-- comment\n"use strict";')).toBeTrue();
    expect(isStrictFunction('\n--> comment\n"use strict";')).toBeTrue();
    expect(isStrictScript('<!-- comment\n"use strict";')).toBeTrue();
    expect(isStrictScript('--> comment\n"use strict";')).toBeTrue();
});

test("a use strict directive after a directive with a line continuation", () => {
    expect(isStrictFunction('"a\\\r\nb"; "use strict";')).toBeTrue();
    expect(isStrictFunction('"a\\\nb"; "use strict";')).toBeTrue();
    expect(isStrictFunction('"a\\\rb"; "use strict";')).toBeTrue();
});

test("a use strict directive applies to what the function has before it", () => {
    expect('function f(eval) { "use strict"; }').not.toEval();
    expect('function f(arguments) { "use strict"; }').not.toEval();
    expect('function f(a, a) { "use strict"; }').not.toEval();
    expect('function f(yield) { "use strict"; }').not.toEval();
    expect('function eval() { "use strict"; }').not.toEval();
    expect('(function arguments() { "use strict"; })').not.toEval();
    expect('(eval) => { "use strict"; }').not.toEval();
    expect('function f(a = 1) { "use strict"; }').not.toEval();
    expect('function f(eval) { "a"; "use strict"; }').not.toEval();
    expect('function f(eval) { "use\\x20strict"; }').toEval();
    expect('function f(eval) { "a".length; "use strict"; }').toEval();
});

test("a use strict directive applies to the directives before it", () => {
    expect('function f() { "\\01"; "use strict"; }').not.toEval();
    expect('function f() { "\\8"; "use strict"; }').not.toEval();
    expect('function f() { "a"; "\\01"; "b"; "use strict"; }').not.toEval();
    expect('"\\01"; "use strict";').not.toEval();
    expect('function f() { "use strict"; "\\01"; }').not.toEval();
    expect('function f() { "\\01"; }').toEval();
    expect('function f() { "\\0"; "use strict"; }').toEval();
    expect('"\\01"; "use\\x20strict";').toEval();
});
