test("normal mode", () => {
    expect(() => {
        NaN = 5; // NaN is a non-writable global variable
    }).not.toThrow();
});

test("strict mode", () => {
    expect(() => {
        "use strict";
        NaN = 5; // NaN is a non-writable global variable
    }).toThrowWithMessage(TypeError, "Cannot write to non-writable property 'NaN'");
});
