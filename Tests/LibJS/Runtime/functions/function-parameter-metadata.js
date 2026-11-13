test("interleaved duplicate parameters keep their capture metadata", () => {
    function f(a, b, a, arguments, c, b) {
        const read = () => [a, c];
        a = "changed";
        return [read(), b, arguments];
    }

    expect(f(1, 2, 3, "parameter", 5, 6)).toEqual([["changed", 5], 6, "parameter"]);
    expect(f(1, 2)).toEqual([["changed", undefined], undefined, undefined]);
});

test("captured duplicate parameters map only their final argument", () => {
    function f(a, b, a, c, b) {
        const read = () => [a, b, c];
        arguments[0] = "first a";
        arguments[1] = "first b";
        arguments[2] = "last a";
        arguments[4] = "last b";
        return read();
    }

    expect(f(1, 2, 3, 4, 5)).toEqual(["last a", "last b", 4]);
    expect(f(1, 2)).toEqual([undefined, undefined, undefined]);
});

test("strict arguments stay separate from captured parameters", () => {
    function f(a, b, c, d) {
        "use strict";
        const read = () => [b, d];
        b = "changed";
        arguments[1] = "argument";
        return [read(), a, c, arguments[1]];
    }

    expect(f(1, 2, 3, 4)).toEqual([["changed", 4], 1, 3, "argument"]);
    expect(f(1)).toEqual([["changed", undefined], 1, undefined, "argument"]);
});

test("indexed parameter metadata preserves duplicate mapped arguments", () => {
    function f(a, b, c, d, e, f, g, a, b) {
        const read = () => [a, b, c];
        arguments[0] = "first a";
        arguments[1] = "first b";
        arguments[7] = "last a";
        arguments[8] = "last b";
        return read();
    }

    expect(f(1, 2, 3, 4, 5, 6, 7, 8, 9)).toEqual(["last a", "last b", 3]);
    expect(f(1, 2)).toEqual([undefined, undefined, undefined]);
});
