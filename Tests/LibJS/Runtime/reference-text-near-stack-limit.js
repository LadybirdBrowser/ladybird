test("the text of a long chain of calls in an error near the call stack size limit", () => {
    const o = {
        m() {
            return o;
        },
    };
    const callee = new Function("o", "return o" + ".m()".repeat(200) + ".nope();");
    const base = new Function("o", "return o" + ".m()".repeat(200) + ".nope.x;");
    const calleeMessage = "undefined is not a function (evaluated from '<object>.nope')";
    const baseMessage = 'Cannot access property "x" on undefined object ".nope"';
    expect(() => callee(o)).toThrowWithMessage(TypeError, calleeMessage);
    expect(() => base(o)).toThrowWithMessage(TypeError, baseMessage);

    const messages = new Set();
    let levels = 0;
    function recurse() {
        try {
            Reflect.apply(recurse, null, []);
        } catch {}
        if (levels++ > 100) return;
        for (const chain of [callee, base]) {
            try {
                chain(o);
            } catch (error) {
                if (error instanceof TypeError) messages.add(error.message);
            }
        }
    }
    recurse();
    expect([...messages].sort()).toEqual([baseMessage, calleeMessage].sort());
});

test("the text of a reference in more brackets than a text keeps track of", () => {
    const a = { b: {} };
    const inBrackets = count => new Function("a", `${"(".repeat(count)}a.b${")".repeat(count)}.c.d;`);
    expect(() => inBrackets(6)(a)).toThrowWithMessage(
        TypeError,
        'Cannot access property "d" on undefined object "a.b.c"'
    );
    expect(() => inBrackets(10)(a)).toThrowWithMessage(
        TypeError,
        'Cannot access property "d" on undefined object ".c"'
    );
});
