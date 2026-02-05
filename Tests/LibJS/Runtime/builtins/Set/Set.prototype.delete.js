test("basic functionality", () => {
    expect(Set.prototype.delete).toHaveLength(1);

    const set = new Set(["a", "b", "c"]);
    expect(set).toHaveSize(3);
    expect(set.delete("b")).toBeTrue();
    expect(set).toHaveSize(2);
    expect(set.delete("b")).toBeFalse();
    expect(set).toHaveSize(2);
});
