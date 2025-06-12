test("length is 2", () => {
    expect(Map.groupBy).toHaveLength(2);
});

describe("errors", () => {
    test("callback must be a function", () => {
        expect(() => {
            Map.groupBy([], undefined);
        }).toThrowWithMessage(TypeError, "undefined is not a function");
    });

    test("null or undefined items value", () => {
        expect(() => {
            Map.groupBy();
        }).toThrowWithMessage(TypeError, "undefined cannot be converted to an object");

        expect(() => {
            Map.groupBy(undefined);
        }).toThrowWithMessage(TypeError, "undefined cannot be converted to an object");

        expect(() => {
            Map.groupBy(null);
        }).toThrowWithMessage(TypeError, "null cannot be converted to an object");
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        const array = [1, 2, 3, 4, 5, 6];
        const visited = [];
        const trueObject = { true: true };
        const falseObject = { false: false };

        const firstResult = Map.groupBy(array, value => {
            visited.push(value);
            return value % 2 === 0 ? trueObject : falseObject;
        });

        expect(visited).toEqual([1, 2, 3, 4, 5, 6]);
        expect(firstResult).toBeInstanceOf(Map);
        expect(firstResult.size).toBe(2);
        expect(firstResult.get(trueObject)).toEqual([2, 4, 6]);
        expect(firstResult.get(falseObject)).toEqual([1, 3, 5]);

        const secondResult = Map.groupBy(array, (_, index) => {
            return index < array.length / 2 ? trueObject : falseObject;
        });

        expect(secondResult).toBeInstanceOf(Map);
        expect(secondResult.size).toBe(2);
        expect(secondResult.get(trueObject)).toEqual([1, 2, 3]);
        expect(secondResult.get(falseObject)).toEqual([4, 5, 6]);
    });

    test("never calls callback with empty array", () => {
        var callbackCalled = 0;
        const result = Map.groupBy([], () => {
            callbackCalled++;
        });
        expect(result).toBeInstanceOf(Map);
        expect(result.size).toBe(0);
        expect(callbackCalled).toBe(0);
    });

    test("calls callback once for every item", () => {
        var callbackCalled = 0;
        const result = Map.groupBy([1, 2, 3], () => {
            callbackCalled++;
        });
        expect(result).toBeInstanceOf(Map);
        expect(result.size).toBe(1);
        expect(result.get(undefined)).toEqual([1, 2, 3]);
        expect(callbackCalled).toBe(3);
    });

    test("still returns a Map even if the global Map constructor was changed", () => {
        const mapGroupBy = Map.groupBy;
        globalThis.Map = null;
        const result = mapGroupBy([1, 2], value => {
            return value % 2 === 0;
        });
        expect(result.size).toBe(2);
        expect(result.get(true)).toEqual([2]);
        expect(result.get(false)).toEqual([1]);
    });
});
