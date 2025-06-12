test("length is 2", () => {
    expect(Reflect.get).toHaveLength(2);
});

describe("errors", () => {
    test("target must be an object", () => {
        [null, undefined, "foo", 123, NaN, Infinity].forEach(value => {
            expect(() => {
                Reflect.get(value);
            }).toThrowWithMessage(TypeError, `${value} is not an object`);
        });
    });
});

describe("normal behavior", () => {
    test("regular object", () => {
        expect(Reflect.get({})).toBeUndefined();
        expect(Reflect.get({ undefined: 1 })).toBe(1);
        expect(Reflect.get({ foo: 1 })).toBeUndefined();
        expect(Reflect.get({ foo: 1 }, "foo")).toBe(1);
    });

    test("array", () => {
        expect(Reflect.get([])).toBeUndefined();
        expect(Reflect.get([1, 2, 3])).toBeUndefined();
        expect(Reflect.get([1, 2, 3], "0")).toBe(1);
        expect(Reflect.get([1, 2, 3], 0)).toBe(1);
        expect(Reflect.get([1, 2, 3], 1)).toBe(2);
        expect(Reflect.get([1, 2, 3], 2)).toBe(3);
        expect(Reflect.get([1, 2, 3], 4)).toBeUndefined();
    });

    test("string object", () => {
        expect(Reflect.get(new String())).toBeUndefined();
        expect(Reflect.get(new String(), 0)).toBeUndefined();
        expect(Reflect.get(new String("foo"), "0")).toBe("f");
        expect(Reflect.get(new String("foo"), 0)).toBe("f");
        expect(Reflect.get(new String("foo"), 1)).toBe("o");
        expect(Reflect.get(new String("foo"), 2)).toBe("o");
        expect(Reflect.get(new String("foo"), 3)).toBeUndefined();
    });

    test("getter function", () => {
        const foo = {
            get prop() {
                this.getPropCalled = true;
            },
        };
        const bar = {};
        Object.setPrototypeOf(bar, foo);

        expect(foo.getPropCalled).toBeUndefined();
        expect(bar.getPropCalled).toBeUndefined();

        Reflect.get(bar, "prop");
        expect(foo.getPropCalled).toBeUndefined();
        expect(bar.getPropCalled).toBeTrue();

        Reflect.get(bar, "prop", foo);
        expect(foo.getPropCalled).toBeTrue();
        expect(bar.getPropCalled).toBeTrue();
    });

    test("native getter function", () => {
        const typedArray = new Uint8Array(3);
        expect(Reflect.get(Uint8Array.prototype, "length", typedArray)).toBe(3);
    });
});
