test("length is 1", () => {
    expect(Object.freeze).toHaveLength(1);
});

describe("normal behavior", () => {
    test("returns given argument", () => {
        const o = {};
        expect(Object.freeze(42)).toBe(42);
        expect(Object.freeze("foobar")).toBe("foobar");
        expect(Object.freeze(o)).toBe(o);
    });

    test("prevents addition of new properties", () => {
        const o = {};
        expect(o.foo).toBeUndefined();
        Object.freeze(o);
        o.foo = "bar";
        expect(o.foo).toBeUndefined();
    });

    test("prevents deletion of existing properties", () => {
        const o = { foo: "bar" };
        expect(o.foo).toBe("bar");
        Object.freeze(o);
        delete o.foo;
        expect(o.foo).toBe("bar");
    });

    test("prevents changing attributes of existing properties", () => {
        const o = { foo: "bar" };
        Object.freeze(o);
        expect(Object.defineProperty(o, "foo", {})).toBe(o);
        expect(Object.defineProperty(o, "foo", { configurable: false })).toBe(o);
        expect(() => {
            Object.defineProperty(o, "foo", { configurable: true });
        }).toThrowWithMessage(TypeError, "Object's [[DefineOwnProperty]] method returned false");
    });

    test("prevents changing value of existing properties", () => {
        const o = { foo: "bar" };
        expect(o.foo).toBe("bar");
        Object.freeze(o);
        o.foo = "baz";
        expect(o.foo).toBe("bar");
    });

    // #6469
    test("works with indexed properties", () => {
        const a = ["foo"];
        expect(a[0]).toBe("foo");
        Object.freeze(a);
        a[0] = "bar";
        expect(a[0]).toBe("foo");
    });

    test("works with properties that are already non-configurable", () => {
        const o = {};
        Object.defineProperty(o, "foo", {
            value: "bar",
            configurable: false,
            writable: true,
            enumerable: true,
        });
        expect(o.foo).toBe("bar");
        Object.freeze(o);
        o.foo = "baz";
        expect(o.foo).toBe("bar");
    });
});

test("does not override frozen function name", () => {
    const func = Object.freeze(function () {
        return 12;
    });
    const obj = Object.freeze({ name: func });
    expect(obj.name()).toBe(12);
});

test("freeze with huge number of properties doesn't crash", () => {
    const o = {};
    for (let i = 0; i < 50_000; ++i) {
        o["prop" + i] = 1;
    }
    Object.freeze(o);
});

test("freeze with TypedArray", () => {
    const TYPED_ARRAYS = [
        Uint8Array,
        Uint8ClampedArray,
        Uint16Array,
        Uint32Array,
        Int8Array,
        Int16Array,
        Int32Array,
        Float16Array,
        Float32Array,
        Float64Array,
    ];

    const buffer = new ArrayBuffer(5, { maxByteLength: 10 });

    TYPED_ARRAYS.forEach(T => {
        const typedArray = new T(buffer, 0, 0);

        expect(() => {
            Object.freeze(typedArray);
        }).toThrowWithMessage(TypeError, "Could not freeze object");
    });
});

test("objects of the same shape frozen one after another", () => {
    const make = i => ({ a: i, b: "b", [Symbol.iterator]: null });
    for (let i = 0; i < 5; ++i) {
        const object = Object.freeze(make(i));
        expect(Object.isFrozen(object)).toBeTrue();
        expect(Object.isExtensible(object)).toBeFalse();
        object.a = 42;
        expect(object.a).toBe(i);
        expect(Object.getOwnPropertyDescriptor(object, "b")).toEqual({
            value: "b",
            writable: false,
            enumerable: true,
            configurable: false,
        });
        expect(delete object.a).toBeFalse();
    }

    // Objects of the same shape that are not frozen keep their attributes.
    const unfrozen = make(1);
    unfrozen.a = 2;
    expect(unfrozen.a).toBe(2);
    expect(Object.isFrozen(unfrozen)).toBeFalse();

    // Sealing an object of the same shape keeps its properties writable.
    const sealed = Object.seal(make(3));
    sealed.a = 4;
    expect(sealed.a).toBe(4);
    expect(Object.isSealed(sealed)).toBeTrue();
    expect(Object.isFrozen(sealed)).toBeFalse();
    expect(Object.isFrozen(Object.freeze(sealed))).toBeTrue();
});

test("objects with accessors, non-enumerable and already frozen properties", () => {
    let value = 1;
    const withAccessor = {
        get a() {
            return value;
        },
        set a(v) {
            value = v;
        },
        b: 2,
    };
    Object.freeze(withAccessor);
    withAccessor.a = 5;
    expect(value).toBe(5);
    expect(Object.getOwnPropertyDescriptor(withAccessor, "a").configurable).toBeFalse();
    expect(Object.isFrozen(withAccessor)).toBeTrue();

    const partly = { a: 1 };
    Object.defineProperty(partly, "b", { value: 2, writable: false, enumerable: false, configurable: true });
    Object.freeze(partly);
    expect(Object.getOwnPropertyDescriptor(partly, "b")).toEqual({
        value: 2,
        writable: false,
        enumerable: false,
        configurable: false,
    });
    expect(Object.isFrozen(partly)).toBeTrue();

    const empty = Object.freeze([]);
    expect(Object.isFrozen(empty)).toBeTrue();
    expect(() => {
        "use strict";
        empty.push(1);
    }).toThrow(TypeError);
    expect(Object.getOwnPropertyDescriptor(empty, "length").writable).toBeFalse();
});
