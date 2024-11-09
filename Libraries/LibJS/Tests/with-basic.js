test("basic with statement functionality", () => {
    var object = { foo: 5, bar: 6, baz: 7 };
    var qux = 1;

    var bar = 99;

    with (object) {
        expect(foo).toBe(5);
        expect(bar).toBe(6);
        expect(baz).toBe(7);
        expect(qux).toBe(1);
        expect(typeof quz).toBe("undefined");

        bar = 2;
    }

    expect(object.bar).toBe(2);
    expect(() => foo).toThrowWithMessage(ReferenceError, "'foo' is not defined");

    expect(bar).toBe(99);
});

test("syntax error in strict mode", () => {
    expect("'use strict'; with (foo) {}").not.toEval();
});

test("restores lexical environment even when exception is thrown", () => {
    var object = {
        foo: 1,
        get bar() {
            throw Error();
        },
    };

    try {
        with (object) {
            expect(foo).toBe(1);
            bar;
        }
        expect().fail();
    } catch (e) {
        expect(() => foo).toThrowWithMessage(ReferenceError, "'foo' is not defined");
    }
    expect(() => foo).toThrowWithMessage(ReferenceError, "'foo' is not defined");
});
