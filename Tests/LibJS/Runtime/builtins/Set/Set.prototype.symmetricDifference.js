describe("errors", () => {
    test("called with negative size", () => {
        expect(() => {
            new Set().symmetricDifference({ size: -1 });
        }).toThrowWithMessage(RangeError, "size must not be negative");
    });
});

test("basic functionality", () => {
    expect(Set.prototype.symmetricDifference).toHaveLength(1);

    const set1 = new Set(["a", "b", "c"]);
    const set2 = new Set(["b", "c", "d", "e"]);
    const symmetricDifference1to2 = set1.symmetricDifference(set2);
    const symmetricDifference2to1 = set2.symmetricDifference(set1);
    for (const symmetricDifferenceSet of [symmetricDifference1to2, symmetricDifference2to1]) {
        expect(symmetricDifferenceSet).toHaveSize(3);
        ["a", "d", "e"].forEach(value => expect(symmetricDifferenceSet.has(value)).toBeTrue());
    }
});
