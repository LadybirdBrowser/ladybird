test("length is 1", () => {
    expect(Reflect.ownKeys).toHaveLength(1);
});

describe("errors", () => {
    test("target must be an object", () => {
        [null, undefined, "foo", 123, NaN, Infinity].forEach(value => {
            expect(() => {
                Reflect.ownKeys(value);
            }).toThrowWithMessage(TypeError, `${value} is not an object`);
        });
    });
});

describe("normal behavior", () => {
    test("regular empty object has no own keys", () => {
        var objectOwnKeys = Reflect.ownKeys({});
        expect(objectOwnKeys instanceof Array).toBeTrue();
        expect(objectOwnKeys).toHaveLength(0);
    });

    test("regular object with some properties has own keys", () => {
        var symbol = Symbol("symbol");
        var objectOwnKeys = Reflect.ownKeys({ foo: "bar", [symbol]: "qux", bar: "baz", 0: 42 });
        expect(objectOwnKeys instanceof Array).toBeTrue();
        expect(objectOwnKeys).toHaveLength(4);
        expect(objectOwnKeys[0]).toBe("0");
        expect(objectOwnKeys[1]).toBe("foo");
        expect(objectOwnKeys[2]).toBe("bar");
        expect(objectOwnKeys[3]).toBe(symbol);
    });

    test("empty array has only 'length' own key", () => {
        var arrayOwnKeys = Reflect.ownKeys([]);
        expect(arrayOwnKeys instanceof Array).toBeTrue();
        expect(arrayOwnKeys).toHaveLength(1);
        expect(arrayOwnKeys[0]).toBe("length");
    });

    test("array with some values has 'length' and indices own keys", () => {
        var arrayOwnKeys = Reflect.ownKeys(["foo", [], 123, undefined]);
        expect(arrayOwnKeys instanceof Array).toBeTrue();
        expect(arrayOwnKeys).toHaveLength(5);
        expect(arrayOwnKeys[0]).toBe("0");
        expect(arrayOwnKeys[1]).toBe("1");
        expect(arrayOwnKeys[2]).toBe("2");
        expect(arrayOwnKeys[3]).toBe("3");
        expect(arrayOwnKeys[4]).toBe("length");
    });
});
