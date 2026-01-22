test("basic functionality", () => {
    expect(Set.prototype.clear).toHaveLength(0);

    const set = new Set(["a", "b", "c"]);
    expect(set).toHaveSize(3);
    set.clear();
    expect(set).toHaveSize(0);
});
