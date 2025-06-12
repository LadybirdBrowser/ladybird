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
});
