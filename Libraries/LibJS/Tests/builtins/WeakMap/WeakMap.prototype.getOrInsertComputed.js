const a = new String("a");
const b = new String("b");

describe("errors", () => {
    test("called on non-WeakMap object", () => {
        expect(() => {
            WeakMap.prototype.getOrInsertComputed(a, () => 0);
        }).toThrowWithMessage(TypeError, "Not an object of type WeakMap");
    });

    test("key cannot be held weakly", () => {
        const map = new WeakMap();

        [-100, Infinity, NaN, "hello", 152n].forEach(key => {
            const suffix = typeof key === "bigint" ? "n" : "";

            expect(() => {
                map.getOrInsertComputed(key, 1);
            }).toThrowWithMessage(TypeError, `${key}${suffix} cannot be held weakly`);
        });
    });

    test("called with non-function", () => {
        expect(() => {
            new WeakMap().getOrInsertComputed(a, 1);
        }).toThrowWithMessage(TypeError, "1 is not a function");
    });

    test("callback function throws", () => {
        expect(() => {
            new WeakMap().getOrInsertComputed(a, () => {
                throw Error(":^)");
            });
        }).toThrowWithMessage(Error, ":^)");
    });
});

describe("correct behavior", () => {
    test("length is 2", () => {
        expect(WeakMap.prototype.getOrInsertComputed).toHaveLength(2);
    });

    test("inserts new value", () => {
        const map = new WeakMap();

        let result = map.getOrInsertComputed(a, () => 2);
        expect(result).toBe(2);

        result = map.getOrInsertComputed(b, () => 4);
        expect(result).toBe(4);
    });

    test("does not overwrite existing value", () => {
        const map = new WeakMap([[a, 2]]);

        let result = map.getOrInsertComputed(a, () => 3);
        expect(result).toBe(2);
    });

    test("does not invoke callback if already existing", () => {
        const map = new WeakMap([[a, 2]]);
        let invoked = false;

        let result = map.getOrInsertComputed(a, () => {
            invoked = true;
        });
        expect(invoked).toBeFalse();
    });
});
