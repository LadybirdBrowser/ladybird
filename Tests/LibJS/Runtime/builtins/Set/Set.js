test("constructor properties", () => {
    expect(Set).toHaveLength(0);
    expect(Set.name).toBe("Set");
});

describe("errors", () => {
    test("invalid array iterators", () => {
        [-100, Infinity, NaN, {}, 152n].forEach(value => {
            expect(() => {
                new Set(value);
            }).toThrowWithMessage(TypeError, "is not iterable");
        });
    });
    test("called without new", () => {
        expect(() => {
            Set();
        }).toThrowWithMessage(TypeError, "Set constructor must be called with 'new'");
    });
});

describe("normal behavior", () => {
    test("typeof", () => {
        expect(typeof new Set()).toBe("object");
    });

    test("constructor with single array argument", () => {
        var a = new Set([0, 1, 2]);
        expect(a instanceof Set).toBeTrue();
        expect(a).toHaveSize(3);
        var seen = [false, false, false];
        a.forEach(x => {
            seen[x] = true;
        });
        expect(seen[0] && seen[1] && seen[2]);
    });
});
