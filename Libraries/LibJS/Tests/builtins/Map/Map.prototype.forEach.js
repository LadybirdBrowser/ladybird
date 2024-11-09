test("length is 1", () => {
    expect(Map.prototype.forEach).toHaveLength(1);
});

describe("errors", () => {
    test("requires at least one argument", () => {
        expect(() => {
            new Map().forEach();
        }).toThrowWithMessage(TypeError, "undefined is not a function");
    });

    test("callback must be a function", () => {
        expect(() => {
            new Map().forEach(undefined);
        }).toThrowWithMessage(TypeError, "undefined is not a function");
    });
});

describe("normal behavior", () => {
    test("never calls callback with empty set", () => {
        var callbackCalled = 0;
        expect(
            new Map().forEach(() => {
                callbackCalled++;
            })
        ).toBeUndefined();
        expect(callbackCalled).toBe(0);
    });

    test("calls callback once for every item", () => {
        var callbackCalled = 0;
        expect(
            new Map([
                ["a", 0],
                ["b", 1],
                ["c", 2],
            ]).forEach(() => {
                callbackCalled++;
            })
        ).toBeUndefined();
        expect(callbackCalled).toBe(3);
    });

    test("callback receives value, key and map", () => {
        var a = new Map([
            ["a", 0],
            ["b", 1],
            ["c", 2],
        ]);
        a.forEach((value, key, map) => {
            expect(a.has(key)).toBeTrue();
            expect(a.get(key)).toBe(value);
            expect(map).toBe(a);
        });
    });
});
