describe("errors", () => {
    test("called on non-Map object", () => {
        expect(() => {
            Map.prototype.getOrInsertComputed(1, () => 0);
        }).toThrowWithMessage(TypeError, "Not an object of type Map");
    });

    test("called with non-function", () => {
        expect(() => {
            new Map().getOrInsertComputed(1, 1);
        }).toThrowWithMessage(TypeError, "1 is not a function");
    });

    test("callback function throws", () => {
        expect(() => {
            new Map().getOrInsertComputed(1, () => {
                throw Error(":^)");
            });
        }).toThrowWithMessage(Error, ":^)");
    });
});

describe("correct behavior", () => {
    test("length is 2", () => {
        expect(Map.prototype.getOrInsertComputed).toHaveLength(2);
    });

    test("inserts new value", () => {
        const map = new Map();

        let result = map.getOrInsertComputed(1, () => 2);
        expect(result).toBe(2);

        result = map.getOrInsertComputed(3, () => 4);
        expect(result).toBe(4);
    });

    test("does not overwrite existing value", () => {
        const map = new Map([[1, 2]]);

        let result = map.getOrInsertComputed(1, () => 3);
        expect(result).toBe(2);
    });

    test("does not invoke callback if already existing", () => {
        const map = new Map([[1, 2]]);
        let invoked = false;

        let result = map.getOrInsertComputed(1, () => {
            invoked = true;
        });
        expect(invoked).toBeFalse();
    });
});
