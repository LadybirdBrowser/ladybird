test("length is 1", () => {
    expect(Array.prototype.find).toHaveLength(1);
});

describe("errors", () => {
    test("callback must be a function", () => {
        expect(() => {
            [].find(undefined);
        }).toThrowWithMessage(TypeError, "undefined is not a function");
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        var array = ["hello", "friends", 1, 2, false];

        expect(array.find(value => value === "hello")).toBe("hello");
        expect(array.find((value, index, arr) => index === 1)).toBe("friends");
        expect(array.find(value => value == "1")).toBe(1);
        expect(array.find(value => value === 1)).toBe(1);
        expect(array.find(value => typeof value !== "string")).toBe(1);
        expect(array.find(value => typeof value === "boolean")).toBeFalse();
        expect(array.find(value => value > 1)).toBe(2);
        expect(array.find(value => value > 1 && value < 3)).toBe(2);
        expect(array.find(value => value > 100)).toBeUndefined();
        expect([].find(value => value === 1)).toBeUndefined();
    });

    test("never calls callback with empty array", () => {
        var callbackCalled = 0;
        expect(
            [].find(() => {
                callbackCalled++;
            })
        ).toBeUndefined();
        expect(callbackCalled).toBe(0);
    });

    test("calls callback once for every item", () => {
        var callbackCalled = 0;
        expect(
            [1, 2, 3].find(() => {
                callbackCalled++;
            })
        ).toBeUndefined();
        expect(callbackCalled).toBe(3);
    });

    test("empty slots are treated as undefined", () => {
        var callbackCalled = 0;
        expect(
            [1, , , "foo", , undefined, , ,].find(value => {
                callbackCalled++;
                return value === undefined;
            })
        ).toBeUndefined();
        expect(callbackCalled).toBe(2);
    });
});

test("is unscopable", () => {
    expect(Array.prototype[Symbol.unscopables].find).toBeTrue();
    const array = [];
    with (array) {
        expect(() => {
            find;
        }).toThrowWithMessage(ReferenceError, "'find' is not defined");
    }
});
