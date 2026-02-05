test("basic functionality", () => {
    const localSym = Symbol("foo");
    const globalSym = Symbol.for("foo");

    expect(localSym).not.toBe(globalSym);
    expect(localSym).not.toBe(Symbol("foo"));
    expect(globalSym).not.toBe(Symbol("foo"));
    expect(globalSym).toBe(Symbol.for("foo"));
    expect(localSym.toString()).toBe("Symbol(foo)");
    expect(globalSym.toString()).toBe("Symbol(foo)");

    expect(Symbol.for(1).description).toBe("1");
    expect(Symbol.for(true).description).toBe("true");
    expect(Symbol.for({}).description).toBe("[object Object]");
    expect(Symbol.for().description).toBe("undefined");
    expect(Symbol.for(null).description).toBe("null");
});

test("symbol argument throws an error", () => {
    expect(() => {
        Symbol.for(Symbol());
    }).toThrowWithMessage(TypeError, "Cannot convert symbol to string");
});
