describe("normal behavior", () => {
    test("length is 2", () => {
        expect(Array.prototype.with).toHaveLength(2);
    });

    test("basic functionality", () => {
        const a = [1, 2, 3, 4, 5];
        const values = [
            [0, "foo", ["foo", 2, 3, 4, 5]],
            [-5, "foo", ["foo", 2, 3, 4, 5]],
            [4, "foo", [1, 2, 3, 4, "foo"]],
            [-1, "foo", [1, 2, 3, 4, "foo"]],
        ];
        for (const [index, value, expected] of values) {
            const b = a.with(index, value);
            expect(a).not.toBe(b);
            expect(a).toEqual([1, 2, 3, 4, 5]);
            expect(b).toEqual(expected);
        }
    });
});

describe("errors", () => {
    test("null or undefined this value", () => {
        expect(() => {
            Array.prototype.with.call();
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");

        expect(() => {
            Array.prototype.with.call(undefined);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");

        expect(() => {
            Array.prototype.with.call(null);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
    });

    test("out of range index", () => {
        expect(() => {
            [].with(0, "foo");
        }).toThrowWithMessage(RangeError, "Index 0 is out of range of array length 0");
        expect(() => {
            [].with(-1, "foo");
        }).toThrowWithMessage(RangeError, "Index -1 is out of range of array length 0");
    });
});
