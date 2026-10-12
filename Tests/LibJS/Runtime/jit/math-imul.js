test("compiled Math.imul calls keep the low 32 bits of the product", () => {
    const imul = Math.imul;
    const multiply = new Function("imul", "a", "b", "return imul(a, b);");
    jit.prepare(multiply);
    for (let i = 0; i < 10; ++i) multiply(imul, i, 3);
    expect(jit.compile(multiply)).toBe(jit.enabled);
    for (const [a, b] of [
        [3, 4],
        [-1, 8],
        [0x7fffffff, 2],
        [0x7fffffff, 0x7fffffff],
        [-0x80000000, -1],
        [65537, 65537],
        [0, -5],
    ])
        expect(multiply(imul, a, b)).toBe(Math.imul(a, b));
    expect(jit.exitCount(multiply)).toBe(0);
});

test("compiled Math.imul calls convert other arguments like Math.imul", () => {
    const imul = Math.imul;
    const multiply = new Function("imul", "a", "b", "return imul(a, b);");
    jit.prepare(multiply);
    for (let i = 0; i < 10; ++i) multiply(imul, i, 3);
    jit.compile(multiply);
    expect(multiply(imul, 2.5, 3)).toBe(6);
    expect(multiply(imul, "7", 3)).toBe(21);
    expect(multiply(imul, 1e10, 3)).toBe(Math.imul(1e10, 3));
    expect(multiply(imul, undefined, 3)).toBe(0);
    expect(multiply(Math.max, 2, 3)).toBe(3);
});
