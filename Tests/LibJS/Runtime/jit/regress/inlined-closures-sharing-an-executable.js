// A function created in an inlined call of a closure must get that closure's environment, even where another closure
// of the same function is inlined in the same compiled code.

function make(prefix, suffix) {
    return function (value) {
        const join = suffix ? rest => `${prefix}/${suffix}/${rest}` : null;
        return join ? join(value) : `${prefix}/-/${value}`;
    };
}

test("closures of one function inlined in one caller", () => {
    const callees = { plain: make("plain"), withSuffix: make("with", "suffix") };
    function caller(value) {
        if (value === undefined) return callees.plain("x");
        return callees.withSuffix(value);
    }
    jit.prepare(caller);
    for (let i = 0; i < 10; ++i) {
        expect(caller()).toBe("plain/-/x");
        expect(caller("y")).toBe("with/suffix/y");
    }
    jit.compile(caller);
    expect(caller()).toBe("plain/-/x");
    expect(caller("y")).toBe("with/suffix/y");
});
