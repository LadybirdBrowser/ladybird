describe("errors", () => {
    test("called with negative size", () => {
        expect(() => {
            new Set().difference({ size: -1 });
        }).toThrowWithMessage(RangeError, "size must not be negative");
    });
});

test("basic functionality", () => {
    expect(Set.prototype.difference).toHaveLength(1);

    const set1 = new Set(["a", "b", "c"]);
    const set2 = new Set(["b", "c", "d", "e"]);
    const difference1to2 = set1.difference(set2);
    expect(difference1to2).toHaveSize(1);
    expect(difference1to2.has("a")).toBeTrue();
    const difference2to1 = set2.difference(set1);
    expect(difference2to1).toHaveSize(2);
    ["d", "e"].forEach(value => expect(difference2to1.has(value)).toBeTrue());
});
