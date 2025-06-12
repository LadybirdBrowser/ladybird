describe("errors", () => {
    test("called with negative size", () => {
        expect(() => {
            new Set().intersection({ size: -1 });
        }).toThrowWithMessage(RangeError, "size must not be negative");
    });
});

test("basic functionality", () => {
    expect(Set.prototype.intersection).toHaveLength(1);

    const set1 = new Set(["a", "b", "c"]);
    const set2 = new Set(["b", "c", "d", "e"]);
    const intersection1to2 = set1.intersection(set2);
    const intersection2to1 = set2.intersection(set1);
    for (const intersectionSet of [intersection1to2, intersection2to1]) {
        expect(intersectionSet).toHaveSize(2);
        ["b", "c"].forEach(value => expect(intersectionSet.has(value)).toBeTrue());
    }
});
