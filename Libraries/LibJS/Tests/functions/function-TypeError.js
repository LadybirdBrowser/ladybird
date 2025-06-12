test("calling non-function", () => {
    expect(() => {
        const a = true;
        a();
    }).toThrowWithMessage(TypeError, "true is not a function (evaluated from 'a')");
});

test("calling number", () => {
    expect(() => {
        const a = 100 + 20 + 3;
        a();
    }).toThrowWithMessage(TypeError, "123 is not a function (evaluated from 'a')");
});

test("calling undefined object key", () => {
    expect(() => {
        const o = {};
        o.a();
    }).toThrowWithMessage(TypeError, "undefined is not a function (evaluated from 'o.a')");
});

test("calling object", () => {
    expect(() => {
        Math();
    }).toThrowWithMessage(
        TypeError,
        "[object MathObject] is not a function (evaluated from 'Math')"
    );
});

test("constructing object", () => {
    expect(() => {
        new Math();
    }).toThrowWithMessage(
        TypeError,
        "[object MathObject] is not a constructor (evaluated from 'Math')"
    );
});

test("constructing native function", () => {
    expect(() => {
        new isNaN();
    }).toThrowWithMessage(
        TypeError,
        "[object NativeFunction] is not a constructor (evaluated from 'isNaN')"
    );
});
