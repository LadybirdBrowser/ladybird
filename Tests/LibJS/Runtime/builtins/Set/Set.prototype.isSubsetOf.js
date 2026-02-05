describe("errors", () => {
    test("called with negative size", () => {
        expect(() => {
            new Set().isSubsetOf({ size: -1 });
        }).toThrowWithMessage(RangeError, "size must not be negative");
    });
});

test("basic functionality", () => {
    expect(Set.prototype.isSubsetOf).toHaveLength(1);

    const set1 = new Set(["a", "b", "c"]);
    const set2 = new Set(["b", "c"]);
    expect(set1.isSubsetOf(set2)).toBeFalse();
    expect(set2.isSubsetOf(set1)).toBeTrue();
});
