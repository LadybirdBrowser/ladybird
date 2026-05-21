test("basic functionality", () => {
    expect(function () {}.name).toBe("");

    function bar() {}
    expect(bar.name).toBe("bar");
    expect((bar.name = "baz")).toBe("baz");
    expect(bar.name).toBe("bar");
});

test("function assigned to variable", () => {
    let foo = function () {};
    expect(foo.name).toBe("foo");
    expect((foo.name = "bar")).toBe("bar");
    expect(foo.name).toBe("foo");

    let a, b;
    a = b = function () {};
    expect(a.name).toBe("b");
    expect(b.name).toBe("b");
});

test("functions in array assigned to variable", () => {
    const arr = [function () {}, function () {}, function () {}];
    expect(arr[0].name).toBe("");
    expect(arr[1].name).toBe("");
    expect(arr[2].name).toBe("");
});

test("functions in objects", () => {
    let f;
    let o = { a: function () {} };

    expect(o.a.name).toBe("a");
    f = o.a;
    expect(f.name).toBe("a");
    expect(o.a.name).toBe("a");

    o = { ...o, b: f };
    expect(o.a.name).toBe("a");
    expect(o.b.name).toBe("a");

    // Member expressions do not get named.
    o.c = function () {};
    expect(o.c.name).toBe("");
});

test("computed property names infer anonymous function names", () => {
    const namedSymbol = Symbol("named");
    const emptySymbol = Symbol("");
    const unnamedSymbol = Symbol();
    const getterSymbol = Symbol("getter");
    const setterSymbol = Symbol("");

    const object = {
        [1]: function () {},
        [2]: class {},
        [namedSymbol]: () => {},
        [emptySymbol]: function* () {},
        [unnamedSymbol]: async function () {},
        [/a/]: function () {},
        get [getterSymbol]() {},
        set [setterSymbol](value) {},
    };

    expect(object[1].name).toBe("1");
    expect(object[2].name).toBe("2");
    expect(object[namedSymbol].name).toBe("[named]");
    expect(object[emptySymbol].name).toBe("[]");
    expect(object[unnamedSymbol].name).toBe("");
    expect(object[/a/].name).toBe("/a/");
    expect(Object.getOwnPropertyDescriptor(object, getterSymbol).get.name).toBe("get [getter]");
    expect(Object.getOwnPropertyDescriptor(object, setterSymbol).set.name).toBe("set []");
});

test("computed property name inference does not leak between evaluations", () => {
    function objectName(key) {
        return { [key]: function () {} }[key].name;
    }

    function classMethodName(key) {
        return new (class {
            [key]() {}
        })()[key].name;
    }

    expect(objectName("first")).toBe("first");
    expect(objectName("second")).toBe("second");
    expect(classMethodName("first")).toBe("first");
    expect(classMethodName("second")).toBe("second");
});

test("computed property name inference only applies to anonymous definitions", () => {
    const key = "computed";
    const value = [function () {}][0];
    const object = { [key]: value };

    expect(object[key].name).toBe("");
});

test("names of native functions", () => {
    expect(console.debug.name).toBe("debug");
    expect((console.debug.name = "warn")).toBe("warn");
    expect(console.debug.name).toBe("debug");
});

describe("some anonymous functions get renamed", () => {
    test("direct assignment does name new function expression", () => {
        // prettier-ignore
        let f1 = (function () {});
        expect(f1.name).toBe("f1");

        let f2 = false;
        f2 ||= function () {};
        expect(f2.name).toBe("f2");
    });

    test("assignment from variable does not name", () => {
        const f1 = function () {};
        let f3 = f1;
        expect(f3.name).toBe("f1");
    });

    test("assignment via expression does not name", () => {
        let f4 = false || function () {};
        expect(f4.name).toBe("");
    });

    test("parenthesized assignment target does not name", () => {
        let f5;
        eval("(f5) = function () {};");
        expect(f5.name).toBe("");

        let f6;
        eval("(f6) ??= function () {};");
        expect(f6.name).toBe("");

        let f7 = false;
        eval("(f7) ||= function () {};");
        expect(f7.name).toBe("");

        let f8 = true;
        eval("(f8) &&= function () {};");
        expect(f8.name).toBe("");
    });
});
