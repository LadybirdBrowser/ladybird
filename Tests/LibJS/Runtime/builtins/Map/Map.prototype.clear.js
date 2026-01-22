test("basic functionality", () => {
    expect(Map.prototype.clear).toHaveLength(0);

    const map = new Map([
        ["a", 0],
        ["b", 1],
        ["c", 2],
    ]);
    expect(map).toHaveSize(3);
    map.clear();
    expect(map).toHaveSize(0);
});
