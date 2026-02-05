test("length is 2", () => {
    expect(Object.groupBy).toHaveLength(2);
});

describe("errors", () => {
    test("callback must be a function", () => {
        expect(() => {
            Object.groupBy([], undefined);
        }).toThrowWithMessage(TypeError, "undefined is not a function");
    });

    test("null or undefined items value", () => {
        expect(() => {
            Object.groupBy();
        }).toThrowWithMessage(TypeError, "undefined cannot be converted to an object");

        expect(() => {
            Object.groupBy(undefined);
        }).toThrowWithMessage(TypeError, "undefined cannot be converted to an object");

        expect(() => {
            Object.groupBy(null);
        }).toThrowWithMessage(TypeError, "null cannot be converted to an object");
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        const array = [1, 2, 3, 4, 5, 6];
        const visited = [];

        const firstResult = Object.groupBy(array, value => {
            visited.push(value);
            return value % 2 === 0;
        });

        expect(visited).toEqual([1, 2, 3, 4, 5, 6]);
        expect(firstResult.true).toEqual([2, 4, 6]);
        expect(firstResult.false).toEqual([1, 3, 5]);

        const firstKeys = Object.keys(firstResult);
        expect(firstKeys).toHaveLength(2);
        expect(firstKeys[0]).toBe("false");
        expect(firstKeys[1]).toBe("true");

        const secondResult = Object.groupBy(array, (_, index) => {
            return index < array.length / 2;
        });

        expect(secondResult.true).toEqual([1, 2, 3]);
        expect(secondResult.false).toEqual([4, 5, 6]);

        const secondKeys = Object.keys(secondResult);
        expect(secondKeys).toHaveLength(2);
        expect(secondKeys[0]).toBe("true");
        expect(secondKeys[1]).toBe("false");
    });

    test("never calls callback with empty array", () => {
        var callbackCalled = 0;
        expect(
            Object.groupBy([], () => {
                callbackCalled++;
            })
        ).toEqual({});
        expect(callbackCalled).toBe(0);
    });

    test("calls callback once for every item", () => {
        var callbackCalled = 0;
        const result = Object.groupBy([1, 2, 3], () => {
            callbackCalled++;
        });
        expect(result.undefined).toEqual([1, 2, 3]);
        expect(callbackCalled).toBe(3);
    });
});
