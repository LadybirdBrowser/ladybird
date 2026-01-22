const a = new String("a");
const b = new String("b");

describe("errors", () => {
    test("called on non-WeakMap object", () => {
        expect(() => {
            WeakMap.prototype.getOrInsert(a, 1);
        }).toThrowWithMessage(TypeError, "Not an object of type WeakMap");
    });

    test("key cannot be held weakly", () => {
        const map = new WeakMap();

        [-100, Infinity, NaN, "hello", 152n].forEach(key => {
            const suffix = typeof key === "bigint" ? "n" : "";

            expect(() => {
                map.getOrInsert(key, 1);
            }).toThrowWithMessage(TypeError, `${key}${suffix} cannot be held weakly`);
        });
    });
});

describe("correct behavior", () => {
    test("length is 2", () => {
        expect(WeakMap.prototype.getOrInsert).toHaveLength(2);
    });

    test("inserts new value", () => {
        const map = new WeakMap();

        let result = map.getOrInsert(a, 2);
        expect(result).toBe(2);

        result = map.getOrInsert(b, 4);
        expect(result).toBe(4);
    });

    test("does not overwrite existing value", () => {
        const map = new WeakMap([[a, 2]]);

        let result = map.getOrInsert(a, 3);
        expect(result).toBe(2);
    });
});
