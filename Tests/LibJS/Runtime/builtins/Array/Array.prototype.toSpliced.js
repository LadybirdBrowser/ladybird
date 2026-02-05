describe("normal behavior", () => {
    test("length is 2", () => {
        expect(Array.prototype.toSpliced).toHaveLength(2);
    });

    test("no start or delete count argument", () => {
        const a = [1, 2, 3, 4, 5];
        const b = a.toSpliced();
        expect(a).not.toBe(b);
        expect(a).toEqual([1, 2, 3, 4, 5]);
        expect(b).toEqual([1, 2, 3, 4, 5]);
    });

    test("only start argument", () => {
        const a = [1, 2, 3, 4, 5];
        const values = [
            [0, []],
            [1, [1]],
            [4, [1, 2, 3, 4]],
            [-1, [1, 2, 3, 4]],
            [999, [1, 2, 3, 4, 5]],
            [Infinity, [1, 2, 3, 4, 5]],
        ];
        for (const [start, expected] of values) {
            const b = a.toSpliced(start);
            expect(a).not.toBe(b);
            expect(a).toEqual([1, 2, 3, 4, 5]);
            expect(b).toEqual(expected);
        }
    });

    test("start and delete count argument", () => {
        const a = [1, 2, 3, 4, 5];
        const values = [
            [0, 5, []],
            [1, 3, [1, 5]],
            [4, 1, [1, 2, 3, 4]],
            [-1, 1, [1, 2, 3, 4]],
            [999, 10, [1, 2, 3, 4, 5]],
            [Infinity, Infinity, [1, 2, 3, 4, 5]],
        ];
        for (const [start, deleteCount, expected] of values) {
            const b = a.toSpliced(start, deleteCount);
            expect(a).not.toBe(b);
            expect(a).toEqual([1, 2, 3, 4, 5]);
            expect(b).toEqual(expected);
        }
    });

    test("start, delete count, and items argument", () => {
        const a = [1, 2, 3, 4, 5];
        const values = [
            [0, 5, ["foo", "bar"], ["foo", "bar"]],
            [1, 3, ["foo", "bar"], [1, "foo", "bar", 5]],
            [4, 1, ["foo", "bar"], [1, 2, 3, 4, "foo", "bar"]],
            [-1, 1, ["foo", "bar"], [1, 2, 3, 4, "foo", "bar"]],
            [999, 10, ["foo", "bar"], [1, 2, 3, 4, 5, "foo", "bar"]],
            [Infinity, Infinity, ["foo", "bar"], [1, 2, 3, 4, 5, "foo", "bar"]],
        ];
        for (const [start, deleteCount, items, expected] of values) {
            const b = a.toSpliced(start, deleteCount, ...items);
            expect(a).not.toBe(b);
            expect(a).toEqual([1, 2, 3, 4, 5]);
            expect(b).toEqual(expected);
        }
    });

    test("is unscopable", () => {
        expect(Array.prototype[Symbol.unscopables].toSpliced).toBeTrue();
        const array = [];
        with (array) {
            expect(() => {
                toSpliced;
            }).toThrowWithMessage(ReferenceError, "'toSpliced' is not defined");
        }
    });
});

describe("errors", () => {
    test("null or undefined this value", () => {
        expect(() => {
            Array.prototype.toSpliced.call();
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");

        expect(() => {
            Array.prototype.toSpliced.call(undefined);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");

        expect(() => {
            Array.prototype.toSpliced.call(null);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
    });

    test("maximum array size exceeded", () => {
        const a = { length: 2 ** 53 - 1 };
        expect(() => {
            Array.prototype.toSpliced.call(a, 0, 0, "foo");
        }).toThrowWithMessage(TypeError, "Maximum array size exceeded");
    });

    test("invalid array length", () => {
        const a = { length: 2 ** 32 - 1 };
        expect(() => {
            Array.prototype.toSpliced.call(a, 0, 0, "foo");
        }).toThrowWithMessage(RangeError, "Invalid array length");
    });
});
