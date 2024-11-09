test("length is 1", () => {
    expect(Array.prototype.findLast).toHaveLength(1);
});

describe("errors", () => {
    test("callback must be a function", () => {
        expect(() => {
            [].findLast(undefined);
        }).toThrowWithMessage(TypeError, "undefined is not a function");
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        var array = ["hello", "friends", 1, 2, false];

        expect(array.findLast(value => value === "hello")).toBe("hello");
        expect(array.findLast((value, index, arr) => index === 1)).toBe("friends");
        expect(array.findLast(value => value == "1")).toBe(1);
        expect(array.findLast(value => value === 1)).toBe(1);
        expect(array.findLast(value => typeof value !== "string")).toBeFalse();
        expect(array.findLast(value => typeof value === "boolean")).toBeFalse();
        expect(array.findLast(value => typeof value === "string")).toBe("friends");
        expect(array.findLast(value => value > 1)).toBe(2);
        expect(array.findLast(value => value >= 1)).toBe(2);
        expect(array.findLast(value => value > 1 && value < 3)).toBe(2);
        expect(array.findLast(value => value > 100)).toBeUndefined();
        expect([].findLast(value => value === 1)).toBeUndefined();
    });

    test("never calls callback with empty array", () => {
        var callbackCalled = 0;
        expect(
            [].findLast(() => {
                callbackCalled++;
            })
        ).toBeUndefined();
        expect(callbackCalled).toBe(0);
    });

    test("calls callback once for every item", () => {
        var callbackCalled = 0;
        expect(
            [1, 2, 3].findLast(() => {
                callbackCalled++;
            })
        ).toBeUndefined();
        expect(callbackCalled).toBe(3);
    });

    test("empty slots are treated as undefined", () => {
        var callbackCalled = 0;
        expect(
            [1, , , "foo", , undefined, , , 6].findLast(value => {
                callbackCalled++;
                return value === undefined;
            })
        ).toBeUndefined();
        expect(callbackCalled).toBe(2);
    });
});

test("is unscopable", () => {
    expect(Array.prototype[Symbol.unscopables].findLast).toBeTrue();
    const array = [];
    with (array) {
        expect(() => {
            findLast;
        }).toThrowWithMessage(ReferenceError, "'findLast' is not defined");
    }
});
