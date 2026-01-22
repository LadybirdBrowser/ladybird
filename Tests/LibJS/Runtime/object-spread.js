const testObjSpread = obj => {
    expect(obj).toEqual({
        foo: 0,
        bar: 1,
        baz: 2,
        qux: 3,
    });
};

const testObjStrSpread = obj => {
    expect(obj).toEqual({ 0: "a", 1: "b", 2: "c", 3: "d" });
};

test("spread object literal inside object literal", () => {
    const obj = {
        foo: 0,
        ...{ bar: 1, baz: 2 },
        qux: 3,
    };
    testObjSpread(obj);
});

test("spread object with assigned property inside object literal", () => {
    const obj = { foo: 0, bar: 1, baz: 2 };
    obj.qux = 3;
    testObjSpread({ ...obj });
});

test("spread object inside object literal", () => {
    let a = { bar: 1, baz: 2 };
    const obj = { foo: 0, ...a, qux: 3 };
    testObjSpread(obj);
});

test("complex nested object spreading", () => {
    const obj = {
        ...{},
        ...{
            ...{ foo: 0, bar: 1, baz: 2 },
        },
        qux: 3,
    };
    testObjSpread(obj);
});

test("spread string in object literal", () => {
    const obj = { ..."abcd" };
    testObjStrSpread(obj);
});

test("spread array in object literal", () => {
    const obj = { ...["a", "b", "c", "d"] };
    testObjStrSpread(obj);
});

test("spread array with holes in object literal", () => {
    const obj = { ...[, , "a", , , , "b", "c", , "d", , ,] };
    expect(obj).toEqual({ 2: "a", 6: "b", 7: "c", 9: "d" });
});

test("spread string object in object literal", () => {
    const obj = { ...String("abcd") };
    testObjStrSpread(obj);
});

test("spread object with non-enumerable property", () => {
    const a = { foo: 0 };
    Object.defineProperty(a, "bar", {
        value: 1,
        enumerable: false,
    });
    const obj = { ...a };
    expect(obj.foo).toBe(0);
    expect(obj).not.toHaveProperty("bar");
});

test("spread object with symbol keys", () => {
    const s = Symbol("baz");
    const a = {
        foo: "bar",
        [s]: "qux",
    };
    const obj = { ...a };
    expect(obj.foo).toBe("bar");
    expect(obj[s]).toBe("qux");
});

test("spreading non-spreadable values", () => {
    let empty = {
        ...undefined,
        ...null,
        ...1,
        ...true,
        ...function () {},
        ...Date,
    };
    expect(Object.getOwnPropertyNames(empty)).toHaveLength(0);
});

test("respects custom Symbol.iterator method", () => {
    let o = {
        [Symbol.iterator]() {
            return {
                i: 0,
                next() {
                    if (this.i++ == 3) {
                        return { done: true };
                    }
                    return { value: this.i, done: false };
                },
            };
        },
    };

    let a = [...o];
    expect(a).toEqual([1, 2, 3]);
});

test("object with numeric indices", () => {
    const obj = { 0: 0, 1: 1, foo: "bar" };
    const result = { ...obj };
    expect(result).toHaveProperty("0", 0);
    expect(result).toHaveProperty("1", 1);
    expect(result).toHaveProperty("foo", "bar");
});

describe("modification of spreadable objects during spread", () => {
    test("spreading object", () => {
        const object = {
            0: 0,
            2: 2,
            9999: 9999,
            bar: 44,
            get 3() {
                object[4] = 4;
                object[5000] = 5000;
                return 3;
            },
        };

        const result = { ...object };
        expect(Object.getOwnPropertyNames(result)).toHaveLength(5);
        expect(Object.getOwnPropertyNames(result)).not.toContain("4");
        expect(Object.getOwnPropertyNames(result)).toContain("bar");
    });

    test("spreading array", () => {
        const array = [0];
        array[2] = 2;
        array[999] = 999;
        Object.defineProperty(array, 3, {
            get() {
                array[4] = 4;
                array[1000] = 1000;
                return 3;
            },
            enumerable: true,
        });

        const objectResult = { ...array };
        expect(Object.getOwnPropertyNames(objectResult)).toHaveLength(4);
        expect(Object.getOwnPropertyNames(objectResult)).not.toContain("4");

        const arrayResult = [...array];
        expect(arrayResult).toHaveLength(1001);
        expect(arrayResult).toHaveProperty("0", 0);
        expect(arrayResult).toHaveProperty("2", 2);
        expect(arrayResult).toHaveProperty("3", 3);
        // Yes the in flight added items need to be here in this case! (since it uses an iterator)
        expect(arrayResult).toHaveProperty("4", 4);
        expect(arrayResult).toHaveProperty("999", 999);
        expect(arrayResult).toHaveProperty("1000", 1000);
    });
});

test("allows assignment expressions", () => {
    expect("({ ...a = { hello: 'world' } })").toEval();
    expect("({ ...a += 'hello' })").toEval();
    expect("({ ...a -= 'hello' })").toEval();
    expect("({ ...a **= 'hello' })").toEval();
    expect("({ ...a *= 'hello' })").toEval();
    expect("({ ...a /= 'hello' })").toEval();
    expect("({ ...a %= 'hello' })").toEval();
    expect("({ ...a <<= 'hello' })").toEval();
    expect("({ ...a >>= 'hello' })").toEval();
    expect("({ ...a >>>= 'hello' })").toEval();
    expect("({ ...a &= 'hello' })").toEval();
    expect("({ ...a ^= 'hello' })").toEval();
    expect("({ ...a |= 'hello' })").toEval();
    expect("({ ...a &&= 'hello' })").toEval();
    expect("({ ...a ||= 'hello' })").toEval();
    expect("({ ...a ??= 'hello' })").toEval();
    expect("function* test() { return ({ ...yield a }); }").toEval();
});

test("spreading null-proto objects", () => {
    const obj = {
        __proto__: null,
        hello: "world",
        friends: "well hello",
        toString() {
            expect().fail("called toString()");
        },
        valueOf() {
            expect().fail("called valueOf()");
        },
    };
    let res;
    expect(() => {
        res = { ...obj };
    }).not.toThrow();
    expect(res).toHaveProperty("hello", "world");
    expect(res).toHaveProperty("friends", "well hello");
});
