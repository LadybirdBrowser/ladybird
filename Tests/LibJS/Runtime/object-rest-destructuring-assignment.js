describe("object rest destructuring with MemberExpression target", () => {
    test("basic rest with dot notation MemberExpression", () => {
        var t = {};
        ({ a, ...t.rest } = { a: 1, b: 2, c: 3 });
        expect(t.rest).toEqual({ b: 2, c: 3 });
    });

    test("basic rest with nested dot notation MemberExpression", () => {
        var obj = { inner: {} };
        ({ x, ...obj.inner.rest } = { x: 10, y: 20, z: 30 });
        expect(obj.inner.rest).toEqual({ y: 20, z: 30 });
    });

    test("rest with bracket notation MemberExpression", () => {
        var arr = [];
        ({ a, ...arr[0] } = { a: 1, b: 2, c: 3 });
        expect(arr[0]).toEqual({ b: 2, c: 3 });
    });

    test("rest with computed property name MemberExpression", () => {
        var key = "key";
        var t = {};
        ({ a, ...t[key] } = { a: 1, b: 2, c: 3 });
        expect(t.key).toEqual({ b: 2, c: 3 });
    });

    test("rest as only element with MemberExpression", () => {
        var t = {};
        ({ ...t.all } = { a: 1, b: 2 });
        expect(t.all).toEqual({ a: 1, b: 2 });
    });

    test("rest with all properties excluded", () => {
        var t = {};
        ({ a, b, c, ...t.rest } = { a: 1, b: 2, c: 3 });
        expect(t.rest).toEqual({});
    });

    test("rest with multiple exclusions", () => {
        var t = {};
        ({ a, b, c, ...t.rest } = { a: 1, b: 2, c: 3, d: 4, e: 5 });
        expect(t.rest).toEqual({ d: 4, e: 5 });
    });
});

describe("object rest destructuring creates a fresh object", () => {
    test("rest object is not the same reference as the source", () => {
        var source = { a: 1, b: 2 };
        var t = {};
        ({ ...t.rest } = source);
        expect(t.rest).not.toBe(source);
        expect(t.rest).toEqual({ a: 1, b: 2 });
    });

    test("rest object is not the same reference when using simple binding", () => {
        var source = { a: 1, b: 2 };
        var { ...rest } = source;
        expect(rest).not.toBe(source);
        expect(rest).toEqual({ a: 1, b: 2 });
    });
});

describe("object rest destructuring property filtering", () => {
    test("only copies own enumerable properties", () => {
        var proto = { inherited: 99 };
        var source = Object.create(proto);
        source.own = 42;
        var t = {};
        ({ ...t.rest } = source);
        expect(t.rest).toEqual({ own: 42 });
        expect(t.rest.hasOwnProperty("inherited")).toBeFalse();
    });

    test("excludes non-enumerable properties", () => {
        var source = {};
        Object.defineProperty(source, "hidden", { value: 1, enumerable: false });
        source.visible = 2;
        var t = {};
        ({ ...t.rest } = source);
        expect(t.rest).toEqual({ visible: 2 });
        expect(t.rest.hasOwnProperty("hidden")).toBeFalse();
    });

    test("includes symbol properties", () => {
        var sym = Symbol("test");
        var source = { a: 1, [sym]: 2 };
        var t = {};
        ({ a, ...t.rest } = source);
        expect(t.rest[sym]).toBe(2);
    });

    test("getter properties are copied as data properties", () => {
        var source = { a: 1 };
        Object.defineProperty(source, "g", {
            get() {
                return 42;
            },
            enumerable: true,
        });
        var t = {};
        ({ a, ...t.rest } = source);
        expect(t.rest.g).toBe(42);
        var descriptor = Object.getOwnPropertyDescriptor(t.rest, "g");
        expect(descriptor.hasOwnProperty("value")).toBeTrue();
        expect(descriptor.value).toBe(42);
    });

    test("numeric key exclusion", () => {
        var t = {};
        ({ 1: a, ...t.rest } = { 1: "one", 2: "two", 3: "three" });
        expect(t.rest).toEqual({ 2: "two", 3: "three" });
    });
});

describe("object rest destructuring with simple binding", () => {
    test("basic rest into variable", () => {
        var { a, ...rest } = { a: 1, b: 2, c: 3 };
        expect(a).toBe(1);
        expect(rest).toEqual({ b: 2, c: 3 });
    });

    test("rest with aliased properties", () => {
        var { a: x, ...rest } = { a: 1, b: 2, c: 3 };
        expect(x).toBe(1);
        expect(rest).toEqual({ b: 2, c: 3 });
    });

    test("rest with default values", () => {
        var { a = 10, ...rest } = { b: 2, c: 3 };
        expect(a).toBe(10);
        expect(rest).toEqual({ b: 2, c: 3 });
    });

    test("rest with default value overridden by source", () => {
        var { a = 10, ...rest } = { a: 1, b: 2, c: 3 };
        expect(a).toBe(1);
        expect(rest).toEqual({ b: 2, c: 3 });
    });
});
