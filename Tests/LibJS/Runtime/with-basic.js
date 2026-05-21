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

test("with object changes can shadow an outer binding", () => {
    let outer = "outer";
    let object = {};
    let seen = [];

    with (object) {
        for (let i = 0; i < 2; ++i) {
            seen.push(outer);
            object.outer = "object";
        }
    }

    expect(seen).toEqual(["outer", "object"]);
});

test("assignment keeps the resolved with binding through RHS side effects", () => {
    var outer = 0;
    var object = { outer: 1 };

    with (object) {
        outer = (delete object.outer, 2);
    }

    expect(object.outer).toBe(2);
    expect(outer).toBe(0);
});

test("var initializer keeps the resolved with binding through initializer side effects", () => {
    var object = { test262id: 1 };

    with (object) {
        var test262id = delete object.test262id;
    }

    expect(object.test262id).toBe(true);
    expect(test262id).toBeUndefined();
});

test("with statement updates empty abrupt completions to undefined", () => {
    expect(eval("1; do { 2; with ({}) { 3; break; } 4; } while (false);")).toBe(3);
    expect(eval("5; do { 6; with ({}) { break; } 7; } while (false);")).toBeUndefined();
    expect(eval("8; do { 9; with ({}) { 10; continue; } 11; } while (false);")).toBe(10);
    expect(eval("12; do { 13; with ({}) { continue; } 14; } while (false);")).toBeUndefined();
});
