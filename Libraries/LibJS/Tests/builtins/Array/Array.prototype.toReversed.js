describe("normal behavior", () => {
    test("length is 0", () => {
        expect(Array.prototype.toReversed).toHaveLength(0);
    });

    test("basic functionality", () => {
        const a = [1, 2, 3, 4, 5];
        const b = a.toReversed();
        expect(a).not.toBe(b);
        expect(a).toEqual([1, 2, 3, 4, 5]);
        expect(b).toEqual([5, 4, 3, 2, 1]);
    });

    test("is unscopable", () => {
        expect(Array.prototype[Symbol.unscopables].toReversed).toBeTrue();
        const array = [];
        with (array) {
            expect(() => {
                toReversed;
            }).toThrowWithMessage(ReferenceError, "'toReversed' is not defined");
        }
    });
});

describe("errors", () => {
    test("null or undefined this value", () => {
        expect(() => {
            Array.prototype.toReversed.call();
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");

        expect(() => {
            Array.prototype.toReversed.call(undefined);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");

        expect(() => {
            Array.prototype.toReversed.call(null);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
    });
});
