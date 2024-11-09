"use strict";

test("basic functionality", () => {
    [true, false, "foo", 123, 123n, null, undefined].forEach(primitive => {
        let description = `${typeof primitive} '${primitive}${
            typeof primitive == "bigint" ? "n" : ""
        }'`;
        if (primitive == null) description = String(primitive);
        expect(() => {
            primitive.foo = "bar";
        }).toThrowWithMessage(TypeError, `Cannot set property 'foo' of ${description}`);
        expect(() => {
            primitive[Symbol.hasInstance] = 123;
        }).toThrowWithMessage(
            TypeError,
            `Cannot set property 'Symbol(Symbol.hasInstance)' of ${description}`
        );
    });
});
