test("basic functionality", () => {
    const localSym = Symbol("foo");
    const globalSym = Symbol.for("foo");

    expect(Symbol.keyFor(localSym)).toBeUndefined();
    expect(Symbol.keyFor(globalSym)).toBe("foo");
});

test("bad argument values", () => {
    [
        [1, "1"],
        [null, "null"],
        [undefined, "undefined"],
        [[], "[object Array]"],
        [{}, "[object Object]"],
        [true, "true"],
        ["foobar", "foobar"],
        [function () {}, "[object ECMAScriptFunctionObject]"], // FIXME: Better function stringification
    ].forEach(testCase => {
        expect(() => {
            Symbol.keyFor(testCase[0]);
        }).toThrowWithMessage(TypeError, `${testCase[1]} is not a symbol`);
    });
});
