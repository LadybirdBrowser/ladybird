describe("errors", () => {
    test("called with negative size", () => {
        expect(() => {
            new Set().union({ size: -1 });
        }).toThrowWithMessage(RangeError, "size must not be negative");
    });
});

test("basic functionality", () => {
    expect(Set.prototype.union).toHaveLength(1);

    const set1 = new Set(["a", "b", "c"]);
    const set2 = new Set(["b", "c", "d"]);
    const union1to2 = set1.union(set2);
    const union2to1 = set2.union(set1);
    for (const unionSet of [union1to2, union2to1]) {
        expect(unionSet).toHaveSize(4);
        ["a", "b", "c", "d"].forEach(value => expect(unionSet.has(value)).toBeTrue());
    }
});
