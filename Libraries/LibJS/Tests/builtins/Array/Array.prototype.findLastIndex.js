test("length is 1", () => {
    expect(Array.prototype.findLastIndex).toHaveLength(1);
});

describe("errors", () => {
    test("callback must be a function", () => {
        expect(() => {
            [].findLastIndex(undefined);
        }).toThrowWithMessage(TypeError, "undefined is not a function");
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        var array = ["hello", "friends", 1, 2, false];

        expect(array.findLastIndex(value => value === "hello")).toBe(0);
        expect(array.findLastIndex((value, index, arr) => index === 1)).toBe(1);
        expect(array.findLastIndex(value => value == "1")).toBe(2);
        expect(array.findLastIndex(value => value === 1)).toBe(2);
        expect(array.findLastIndex(value => typeof value !== "string")).toBe(4);
        expect(array.findLastIndex(value => typeof value === "boolean")).toBe(4);
        expect(array.findLastIndex(value => typeof value === "string")).toBe(1);
        expect(array.findLastIndex(value => value > 1)).toBe(3);
        expect(array.findLastIndex(value => value >= 1)).toBe(3);
        expect(array.findLastIndex(value => value > 1 && value < 3)).toBe(3);
        expect(array.findLastIndex(value => value > 100)).toBe(-1);
        expect([].findLastIndex(value => value === 1)).toBe(-1);
    });

    test("never calls callback with empty array", () => {
        var callbackCalled = 0;
        expect(
            [].findLastIndex(() => {
                callbackCalled++;
            })
        ).toBe(-1);
        expect(callbackCalled).toBe(0);
    });

    test("calls callback once for every item", () => {
        var callbackCalled = 0;
        expect(
            [1, 2, 3].findLastIndex(() => {
                callbackCalled++;
            })
        ).toBe(-1);
        expect(callbackCalled).toBe(3);
    });

    test("empty slots are treated as undefined", () => {
        var callbackCalled = 0;
        expect(
            [1, , , "foo", , undefined, , , 6].findLastIndex(value => {
                callbackCalled++;
                return value === undefined;
            })
        ).toBe(7);
        expect(callbackCalled).toBe(2);
    });
});

test("is unscopable", () => {
    expect(Array.prototype[Symbol.unscopables].findLastIndex).toBeTrue();
    const array = [];
    with (array) {
        expect(() => {
            findLastIndex;
        }).toThrowWithMessage(ReferenceError, "'findLastIndex' is not defined");
    }
});
