describe("errors", () => {
    test("called on non-Map object", () => {
        expect(() => {
            Map.prototype.getOrInsert(1, 2);
        }).toThrowWithMessage(TypeError, "Not an object of type Map");
    });
});

describe("correct behavior", () => {
    test("length is 2", () => {
        expect(Map.prototype.getOrInsert).toHaveLength(2);
    });

    test("inserts new value", () => {
        const map = new Map();

        let result = map.getOrInsert(1, 2);
        expect(result).toBe(2);

        result = map.getOrInsert(3, 4);
        expect(result).toBe(4);
    });

    test("does not overwrite existing value", () => {
        const map = new Map([[1, 2]]);

        let result = map.getOrInsert(1, 3);
        expect(result).toBe(2);
    });
});
