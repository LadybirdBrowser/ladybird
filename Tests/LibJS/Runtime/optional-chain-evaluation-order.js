test("optional chain call preserves argument evaluation order", () => {
    let a = 1;
    let fn = x => x;
    let result = fn?.(a, (a = 42));
    expect(result).toBe(1);
    expect(a).toBe(42);
});

test("optional chain call with multiple arguments preserving order", () => {
    let x = 10;
    let fn = (a, b) => a + b;
    let result = fn?.(x, ((x = 20), 5));
    expect(result).toBe(15);
    expect(x).toBe(20);
});
