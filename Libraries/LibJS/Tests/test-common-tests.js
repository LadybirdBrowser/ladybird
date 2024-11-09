test("toBe", () => {
    expect(null).toBe(null);
    expect(undefined).toBe(undefined);
    expect(null).not.toBe(undefined);

    expect(1).toBe(1);
    expect(1).not.toBe(2);

    expect("1").toBe("1");
    expect("1").not.toBe("2");

    expect(true).toBeTrue();
    expect(true).not.toBeFalse();

    expect({}).not.toBe({});
    expect([]).not.toBe([]);

    function foo() {}
    expect(foo).toBe(foo);
    expect(function () {}).not.toBe(function () {});

    let s = Symbol("foo");
    expect(s).toBe(s);
    expect(Symbol("foo")).not.toBe(Symbol("foo"));

    expect(1n).toBe(1n);
    expect(1n).not.toBe(1);
});

test("toBeCloseTo", () => {
    expect(1).toBeCloseTo(1);
    expect(1).not.toBeCloseTo(1.1);
    expect(1).not.toBeCloseTo(1.01);
    expect(1).not.toBeCloseTo(1.001);
    expect(1).not.toBeCloseTo(1.0001);
    expect(1).not.toBeCloseTo(1.00001);
    expect(1).toBeCloseTo(1.000001);

    expect(1).not.toBeCloseTo(1.001, 3);
    expect(1).toBeCloseTo(1.001, 2);
    expect(1).not.toBeCloseTo(0, 0);
    expect(6).toBeCloseTo(10, -1);
    expect(125).toBeCloseTo(100, -2);
    expect(3.14159).toBeCloseTo(3.14158, 4);
    expect(() => {
        expect(1).toBeCloseTo(1, "foo").toThrow(ExpectationError);
    }).toThrow(ExpectationError);

    [
        ["foo", 1],
        [1, "foo"],
        [1n, 1],
    ].forEach(arr => {
        expect(() => {
            expect(arr[0]).toBeCloseTo(arr[1]);
        }).toThrow(ExpectationError);
    });
});

test("toHaveLength", () => {
    expect([]).toHaveLength(0);
    expect([]).not.toHaveLength(1);
    expect([1]).toHaveLength(1);
    expect({ length: 1 }).toHaveLength(1);

    expect(
        "a\
b"
    ).toHaveLength(2);

    expect(() => {
        expect(1).toHaveLength();
    }).toThrow(ExpectationError);
});

test("toHaveProperty", () => {
    expect([]).toHaveProperty("length");
    expect([]).toHaveProperty("length", 0);
    expect([1]).not.toHaveProperty("length", 0);
    expect({ foo: "bar" }).toHaveProperty("foo");
    expect({ foo: "bar" }).toHaveProperty("foo", "bar");

    expect({ foo: { bar: "baz" } }).toHaveProperty(["foo", "bar"]);
    expect({ foo: { bar: "baz" } }).toHaveProperty(["foo", "bar"], "baz");
    expect({ foo: { bar: "baz" } }).toHaveProperty("foo.bar");
    expect({ foo: { bar: "baz" } }).toHaveProperty("foo.bar", "baz");

    expect({ foo: { bar: "baz" } }).toHaveProperty(["foo", "bar"]);
    expect({ foo: { bar: "baz" } }).toHaveProperty(["foo", "bar"], "baz");
    expect({ foo: { bar: "baz" } }).not.toHaveProperty(["foo", "baz"]);
    expect({ foo: { bar: "baz" } }).not.toHaveProperty(["foo", "baz"], "qux");
    expect({ foo: { bar: "baz" } }).not.toHaveProperty("foo.baz");
    expect({ foo: { bar: "baz" } }).not.toHaveProperty("foo.baz", "qux");
});

test("toBeDefined", () => {
    expect(1).toBeDefined();
    expect(true).toBeDefined();
    expect(false).toBeDefined();
    expect({}).toBeDefined();
    expect([]).toBeDefined();
    expect("a").toBeDefined();
    expect(null).toBeDefined();
    expect(undefined).not.toBeDefined();
});

test("toBeInstanceOf", () => {
    expect(new Error()).toBeInstanceOf(Error);
    expect(Error).not.toBeInstanceOf(Error);

    class Parent {}
    class Child extends Parent {}

    expect(new Child()).toBeInstanceOf(Child);
    expect(new Child()).toBeInstanceOf(Parent);
    expect(new Parent()).toBeInstanceOf(Parent);
    expect(new Parent()).not.toBeInstanceOf(Child);
});

test("toBeNull", () => {
    expect(null).toBeNull();
    expect(undefined).not.toBeNull();
    expect(5).not.toBeNull();
});

test("toBeUndefined", () => {
    expect(undefined).toBeUndefined();
    expect(null).not.toBeUndefined();
    expect().toBeUndefined();
    expect(5).not.toBeUndefined();
});

test("toBeNaN", () => {
    expect(NaN).toBeNaN();
    expect(5).not.toBeNaN();
});

test("toBeTrue", () => {
    expect(true).toBeTrue();
    expect(false).not.toBeTrue();
    expect(null).not.toBeTrue();
    expect(undefined).not.toBeTrue();
    expect(0).not.toBeTrue();
});

test("toBeFalse", () => {
    expect(true).not.toBeFalse();
    expect(false).toBeFalse();
    expect(null).not.toBeFalse();
    expect(undefined).not.toBeFalse();
    expect(0).not.toBeFalse();
});

test("toBeLessThan", () => {
    expect(0).toBeLessThan(1);
    expect(0).toBeLessThan(0.1);
    expect(1).not.toBeLessThan(1);
    expect(1).not.toBeLessThan(0);

    expect(0n).toBeLessThan(1n);
    expect(1n).not.toBeLessThan(1n);
    expect(1n).not.toBeLessThan(0n);

    [
        ["foo", 0],
        [0, "foo"],
        [0, 0n],
        [0n, 0],
    ].forEach(arr => {
        expect(() => {
            expect(arr[0]).toBeLessThan(arr[1]);
        }).toThrow(ExpectationError);
    });
});

test("toBeLessThanOrEqual", () => {
    expect(0).toBeLessThanOrEqual(1);
    expect(0).toBeLessThanOrEqual(0.1);
    expect(1).toBeLessThanOrEqual(1);
    expect(1).not.toBeLessThanOrEqual(0);

    expect(0n).toBeLessThanOrEqual(1n);
    expect(1n).toBeLessThanOrEqual(1n);
    expect(1n).not.toBeLessThanOrEqual(0n);

    [
        ["foo", 0],
        [0, "foo"],
        [0, 0n],
        [0n, 0],
    ].forEach(arr => {
        expect(() => {
            expect(arr[0]).toBeLessThanOrEqual(arr[1]);
        }).toThrow(ExpectationError);
    });
});

test("toBeGreaterThan", () => {
    expect(1).toBeGreaterThan(0);
    expect(0.1).toBeGreaterThan(0);
    expect(1).not.toBeGreaterThan(1);
    expect(0).not.toBeGreaterThan(1);

    expect(1n).toBeGreaterThan(0n);
    expect(1n).not.toBeGreaterThan(1n);
    expect(0n).not.toBeGreaterThan(1n);

    [
        ["foo", 0],
        [0, "foo"],
        [0, 0n],
        [0n, 0],
    ].forEach(arr => {
        expect(() => {
            expect(arr[0]).toBeGreaterThan(arr[1]);
        }).toThrow(ExpectationError);
    });
});

test("toBeGreaterThanOrEqual", () => {
    expect(1).toBeGreaterThanOrEqual(0);
    expect(0.1).toBeGreaterThanOrEqual(0);
    expect(1).toBeGreaterThanOrEqual(1);
    expect(0).not.toBeGreaterThanOrEqual(1);

    expect(1n).toBeGreaterThanOrEqual(0n);
    expect(1n).toBeGreaterThanOrEqual(1n);
    expect(0n).not.toBeGreaterThanOrEqual(1n);

    [
        ["foo", 0],
        [0, "foo"],
        [0, 0n],
        [0n, 0],
    ].forEach(arr => {
        expect(() => {
            expect(arr[0]).toBeGreaterThanOrEqual(arr[1]);
        }).toThrow(ExpectationError);
    });
});

test("toContain", () => {
    expect([1, 2, 3]).toContain(1);
    expect([1, 2, 3]).toContain(2);
    expect([1, 2, 3]).toContain(3);
    expect([{ foo: 1 }]).not.toContain({ foo: 1 });
});

test("toContainEqual", () => {
    expect([1, 2, 3]).toContainEqual(1);
    expect([1, 2, 3]).toContainEqual(2);
    expect([1, 2, 3]).toContainEqual(3);
    expect([{ foo: 1 }]).toContainEqual({ foo: 1 });
});

test("toEqual", () => {
    expect(undefined).toEqual(undefined);
    expect(null).toEqual(null);
    expect(undefined).not.toEqual(null);
    expect(null).not.toEqual(undefined);
    expect(NaN).toEqual(NaN);

    expect(1).toEqual(1);
    expect("abcd").toEqual("abcd");

    let s = Symbol();
    expect(s).toEqual(s);
    expect(Symbol()).not.toEqual(Symbol());
    expect(Symbol.for("foo")).toEqual(Symbol.for("foo"));

    expect({ foo: 1, bar: { baz: [1, 2, 3] } }).toEqual({ foo: 1, bar: { baz: [1, 2, 3] } });
    expect([1, 2, { foo: 1 }, [3, [4, 5]]]).toEqual([1, 2, { foo: 1 }, [3, [4, 5]]]);

    function foo() {}
    expect(foo).toEqual(foo);
    expect(function () {}).not.toEqual(function () {});
});

test("toThrow", () => {
    expect(() => {}).not.toThrow();
    expect(() => {}).not.toThrow("foo");
    expect(() => {}).not.toThrow(TypeError);
    expect(() => {}).not.toThrow(new TypeError("foo"));

    let thrower = () => {
        throw new TypeError("foo bar");
    };

    expect(thrower).toThrow();
    expect(thrower).toThrow(TypeError);
    expect(thrower).toThrow("o ba");
    expect(thrower).toThrow("foo bar");
    expect(thrower).not.toThrow("baz");
    expect(thrower).not.toThrow(ReferenceError);
    expect(thrower).toThrow(new TypeError("foo bar"));
    expect(thrower).not.toThrow(new TypeError("o ba"));
    expect(thrower).toThrow(new ReferenceError("foo bar"));
    expect(thrower).toThrow({ message: "foo bar" });
});

test("pass", () => {
    expect().pass();
    expect({}).pass();
});

test("fail", () => {
    // FIXME: Doesn't really make sense; this is a great candidate
    // for expect.assertions()
    try {
        expect().fail();
    } catch (e) {
        expect(e.name).toBe("ExpectationError");
    }
});

test("toThrowWithMessage", () => {
    let incorrectUsages = [
        [1, undefined, undefined],
        [() => {}, undefined, undefined],
        [() => {}, function () {}, undefined],
        [() => {}, undefined, "test"],
    ];

    incorrectUsages.forEach(arr => {
        expect(() => {
            expect(arr[0]).toThrowWithMessage(arr[1], arr[2]);
        }).toThrow();
    });

    let thrower = () => {
        throw new TypeError("foo bar");
    };

    expect(thrower).toThrowWithMessage(TypeError, "foo bar");
    expect(thrower).toThrowWithMessage(TypeError, "foo");
    expect(thrower).toThrowWithMessage(TypeError, "o ba");
    expect(thrower).not.toThrowWithMessage(ReferenceError, "foo bar");
    expect(thrower).not.toThrowWithMessage(TypeError, "foo baz");
});

test("toEval", () => {
    expect("let a = 1").toEval();
    expect("a < 1").toEval();
    expect("&&*^%#%@").not.toEval();
    expect("function foo() { return 1; }; foo();").toEval();
});

test("toEvalTo", () => {
    expect("let a = 1").toEvalTo();
    expect("let a = 1").toEvalTo(undefined);
    expect("10").toEvalTo(10);
    expect("10").not.toEvalTo(5);

    expect(() => {
        expect("*^&%%").not.toEvalTo();
    }).toThrow();
});

test("toHaveConfigurableProperty", () => {
    expect({ foo: 1 }).toHaveConfigurableProperty("foo");

    expect(() => {
        expect({ foo: 1 }).not.toHaveConfigurableProperty("bar");
    }).toThrow();

    let o = {};
    Object.defineProperty(o, "foo", { configurable: true, value: 1 });
    Object.defineProperty(o, "bar", { configurable: false, value: 1 });
    expect(o).toHaveConfigurableProperty("foo");
    expect(o).not.toHaveConfigurableProperty("bar");
});

test("toHaveEnumerableProperty", () => {
    expect({ foo: 1 }).toHaveEnumerableProperty("foo");

    expect(() => {
        expect({ foo: 1 }).not.toHaveEnumerableProperty("bar");
    }).toThrow();

    let o = {};
    Object.defineProperty(o, "foo", { enumerable: true, value: 1 });
    Object.defineProperty(o, "bar", { enumerable: false, value: 1 });
    expect(o).toHaveEnumerableProperty("foo");
    expect(o).not.toHaveEnumerableProperty("bar");
});

test("toHaveWritableProperty", () => {
    expect({ foo: 1 }).toHaveWritableProperty("foo");

    expect(() => {
        expect({ foo: 1 }).not.toHaveWritableProperty("bar");
    }).toThrow();

    let o = {};
    Object.defineProperty(o, "foo", { writable: true, value: 1 });
    Object.defineProperty(o, "bar", { writable: false, value: 1 });
    expect(o).toHaveWritableProperty("foo");
    expect(o).not.toHaveWritableProperty("bar");
});

test("toHaveGetterProperty", () => {
    expect(() => {
        expect({ foo: 1 }).not.toHaveGetterProperty("bar");
    }).toThrow();

    let o = {};
    Object.defineProperty(o, "foo", {
        get() {
            return 1;
        },
    });
    Object.defineProperty(o, "bar", { value: 1 });
    expect(o).toHaveGetterProperty("foo");
    expect(o).not.toHaveGetterProperty("bar");
});

test("toHaveSetterProperty", () => {
    expect(() => {
        expect({ foo: 1 }).not.toHaveSetterProperty("bar");
    }).toThrow();

    let o = {};
    Object.defineProperty(o, "foo", { set(_) {} });
    Object.defineProperty(o, "bar", { value: 1 });
    expect(o).toHaveSetterProperty("foo");
    expect(o).not.toHaveSetterProperty("bar");
});
