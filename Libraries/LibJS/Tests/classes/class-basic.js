test("class properties", () => {
    class A {}
    expect(A.name).toBe("A");
    expect(A).toHaveLength(0);
});
