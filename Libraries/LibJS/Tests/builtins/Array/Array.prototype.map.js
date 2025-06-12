test("length is 1", () => {
    expect(Array.prototype.map).toHaveLength(1);
});

describe("errors", () => {
    test("callback must be a function", () => {
        expect(() => {
            [].map(undefined);
        }).toThrowWithMessage(TypeError, "undefined is not a function");
    });
});

describe("normal behavior", () => {
    test("never calls callback with empty array", () => {
        var callbackCalled = 0;
        expect(
            [].map(() => {
                callbackCalled++;
            })
        ).toEqual([]);
        expect(callbackCalled).toBe(0);
    });

    test("calls callback once for every item", () => {
        var callbackCalled = 0;
        expect(
            [1, 2, 3].map(() => {
                callbackCalled++;
            })
        ).toEqual([undefined, undefined, undefined]);
        expect(callbackCalled).toBe(3);
    });

    test("can map based on callback return value", () => {
        expect(
            [undefined, null, true, "foo", 42, {}].map(
                (value, index) => "" + index + " -> " + value
            )
        ).toEqual([
            "0 -> undefined",
            "1 -> null",
            "2 -> true",
            "3 -> foo",
            "4 -> 42",
            "5 -> [object Object]",
        ]);

        var squaredNumbers = [0, 1, 2, 3, 4].map(x => x ** 2);
        expect(squaredNumbers).toEqual([0, 1, 4, 9, 16]);
    });
});
