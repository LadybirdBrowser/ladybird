describe("errors", () => {
    test("called with negative size", () => {
        expect(() => {
            new Set().isSupersetOf({ size: -1 });
        }).toThrowWithMessage(RangeError, "size must not be negative");
    });
});

test("basic functionality", () => {
    expect(Set.prototype.isSupersetOf).toHaveLength(1);

    const set1 = new Set(["a", "b", "c"]);
    const set2 = new Set(["b", "c"]);
    expect(set1.isSupersetOf(set2)).toBeTrue();
    expect(set2.isSupersetOf(set1)).toBeFalse();
});
