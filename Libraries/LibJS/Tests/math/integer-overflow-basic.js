test("basic integer overflow correctness", () => {
    expect(2147483647 + 1).toBe(2147483648);
    expect(2147483648 - 1).toBe(2147483647);
    expect(0 - 2147483647).toBe(-2147483647);
    expect(0 - 2147483648).toBe(-2147483648);
    expect(0 - -2147483647).toBe(2147483647);
    expect(0 - -2147483648).toBe(2147483648);
    expect(0 + -2147483647).toBe(-2147483647);
    expect(0 + -2147483648).toBe(-2147483648);
});
