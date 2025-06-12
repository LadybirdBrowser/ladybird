test("unknown variable produces ReferenceError", () => {
    expect(new Function("i < 3")).toThrow(ReferenceError);
});
