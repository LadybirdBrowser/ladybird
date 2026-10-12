// Checking whether a function has a `prototype` creates the prototype the first time. If the function is the
// prototype of another object, this throws away compiled code that reads properties of that object. A compiled
// function that runs such a check must keep its code. The callee has an unused branch so that it is too large to
// inline, and the caller calls it directly. It gets `hasOwnProperty` as a parameter, because looking up `Object` in
// it would register its frame early and hide the bug. The last call passes an object of another shape, so that the
// callee falls back to the interpreter after the check, which fails if its code is gone.

function makeCallee(parameters, body) {
    return new Function("pad", ...parameters, `if (pad === 1) { ${"pad = pad + 1; ".repeat(16)}} ${body}`);
}

function testPrototypeCheck(check) {
    const callee = makeCallee(["target", "object", "hasOwnProperty"], `return [${check}, object.name];`);
    const caller = (target, object) => callee(undefined, target, object, Object.prototype.hasOwnProperty);
    const target = function target() {};
    const object = Object.create(target);
    jit.prepare(callee);
    jit.prepare(caller);
    for (let i = 0; i < 10; ++i) expect(caller({}, object)).toEqual([false, "target"]);
    jit.compile(callee);
    jit.compile(caller);
    expect(caller(target, { name: "other" })).toEqual([true, "other"]);
}

test("in", () => {
    testPrototypeCheck(`"prototype" in target`);
});

test("hasOwnProperty", () => {
    testPrototypeCheck(`hasOwnProperty.call(target, "prototype")`);
});
