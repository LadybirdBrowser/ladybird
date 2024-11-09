test("constructor properties", () => {
    expect(Map).toHaveLength(0);
    expect(Map.name).toBe("Map");
});

describe("errors", () => {
    test("invalid array iterators", () => {
        [-100, Infinity, NaN, {}, 152n].forEach(value => {
            expect(() => {
                new Map(value);
            }).toThrowWithMessage(TypeError, "is not iterable");
        });
    });
    test("invalid iterator entries", () => {
        expect(() => {
            new Map([1, 2, 3]);
        }).toThrowWithMessage(TypeError, "Iterator value 1 is not an object");
    });
    test("called without new", () => {
        expect(() => {
            Map();
        }).toThrowWithMessage(TypeError, "Map constructor must be called with 'new'");
    });
});

describe("normal behavior", () => {
    test("typeof", () => {
        expect(typeof new Map()).toBe("object");
    });

    test("constructor with single entries array argument", () => {
        var a = new Map([
            ["a", 0],
            ["b", 1],
            ["c", 2],
        ]);
        expect(a instanceof Map).toBeTrue();
        expect(a).toHaveSize(3);
        var seen = [false, false, false];
        a.forEach(v => {
            seen[v] = true;
        });
        expect(seen[0] && seen[1] && seen[2]);
    });
});
