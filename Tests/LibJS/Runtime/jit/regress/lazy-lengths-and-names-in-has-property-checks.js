// A compiled `in` or `hasOwnProperty` check of a function's `length` or `name` must not lose code or corrupt objects.

function makeCallee(parameters, body) {
    return new Function("pad", ...parameters, `if (pad === 1) { ${"pad = pad + 1; ".repeat(100)}} ${body}`);
}

function makeTarget() {
    const target = function target() {};
    target.a = 1;
    target.b = 2;
    return target;
}

function testCheck(check, key) {
    const callee = makeCallee(
        ["target", "key", "object", "hasOwnProperty"],
        `const a = target.a; const result = ${check}; target.b = 3; return [result, a, object.name];`
    );
    const caller = (target, key, object) => callee(undefined, target, key, object, Object.prototype.hasOwnProperty);
    jit.prepare(callee);
    jit.prepare(caller);
    for (let i = 0; i < 10; ++i) expect(caller(makeTarget(), "c", { name: "first" })).toEqual([false, 1, "first"]);
    jit.compile(callee);
    jit.compile(caller);
    const target = makeTarget();
    expect(caller(target, key, { other: 1, name: "second" })).toEqual([true, 1, "second"]);
    expect(target.b).toBe(3);
    expect(target.length).toBe(0);
    expect(target.name).toBe("target");
}

test("in", () => {
    testCheck("key in target", "length");
    testCheck("key in target", "name");
});

test("hasOwnProperty", () => {
    testCheck("hasOwnProperty.call(target, key)", "length");
    testCheck("hasOwnProperty.call(target, key)", "name");
});
