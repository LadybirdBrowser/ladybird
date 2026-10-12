function compiled(source, ...warmupArguments) {
    const parameters = source
        .match(/^\(([^)]*)\)/)[1]
        .split(",")
        .map(parameter => parameter.trim());
    const body = source.slice(source.indexOf("=>") + 2);
    const f = new Function(...parameters, body);
    jit.prepare(f);
    for (let i = 0; i < 10; ++i) f(...warmupArguments);
    expect(jit.compile(f)).toBe(jit.enabled);
    return f;
}

test("sums and differences that only reach ToInt32 wrap without exiting", () => {
    const sum = compiled("(a, b) => return (a + b) | 0;", 1, 2);
    expect(sum(0x7fffffff, 1)).toBe(-0x80000000);
    expect(sum(-0x80000000, -1)).toBe(0x7fffffff);
    expect(sum(-0x80000000, -0x80000000)).toBe(0);

    const difference = compiled("(a, b) => return (a - b) | 0;", 5, 2);
    expect(difference(-0x80000000, 1)).toBe(0x7fffffff);
    expect(difference(0x7fffffff, -1)).toBe(-0x80000000);

    // NB: Like asm.js code, coerce the parameters first: the checks of
    //     parameters read later exit with the exact sums before them.
    const chain = compiled("(a, b, c) => a = a | 0; b = b | 0; c = c | 0; return (a + b + c - a) | 0;", 1, 2, 3);
    expect(chain(0x7fffffff, 0x7fffffff, 0x7fffffff)).toBe((0x7fffffff + 0x7fffffff + 0x7fffffff - 0x7fffffff) | 0);

    const shifted = compiled("(a, b) => return (a + b) >> 1;", 1, 2);
    expect(shifted(0x7fffffff, 0x7fffffff)).toBe(-1);

    const unsigned = compiled("(a, b) => b = b | 0; return ((a >>> 0) + b) | 0;", 1, 2);
    expect(unsigned(-1, 1)).toBe(0);

    for (const f of [sum, difference, chain, shifted, unsigned]) expect(jit.exitCount(f)).toBe(0);
});

test("sums stored to integer typed arrays wrap, clamped ones saturate", () => {
    const store = compiled("(array, a, b) => array[0] = a + b; return array[0];", new Int32Array(1), 1, 2);
    expect(store(new Int32Array(1), 0x7fffffff, 1)).toBe(-0x80000000);
    expect(store(new Int16Array(1), 0x7fffffff, 1)).toBe(0);
    expect(store(new Uint8Array(1), 0x7fffffff, 0x7fffffff)).toBe(254);

    const clamped = compiled("(array, a, b) => array[0] = a + b; return array[0];", new Uint8ClampedArray(1), 1, 2);
    expect(clamped(new Uint8ClampedArray(1), 0x7fffffff, 1)).toBe(255);
    expect(clamped(new Uint8ClampedArray(1), -0x80000000, -1)).toBe(0);
});

test("sums with other users keep their exact values", () => {
    const both = compiled("(a, b) => { const sum = a + b; return [sum | 0, sum]; }", 1, 2);
    expect(both(0x7fffffff, 1)).toEqual([-0x80000000, 0x80000000]);

    const compared = compiled("(a, b) => return (a + b) > 0 ? (a + b) | 0 : 0;", 1, 2);
    expect(compared(0x7fffffff, 1)).toBe(-0x80000000);

    const unsigned = compiled("(a, b) => return (a + b) >>> 0;", 1, 2);
    expect(unsigned(-1, 0)).toBe(0xffffffff);
});

test("loops of asm.js style arithmetic wrap", () => {
    const hash = compiled(
        "(n) => { let h = 0; for (let i = 0; (i | 0) < (n | 0); i = (i + 1) | 0) h = (h + ((h << 5) + i)) | 0; return h; }",
        3
    );
    let expected = 0;
    for (let i = 0; i < 1000; ++i) expected = (expected + ((expected << 5) + i)) | 0;
    expect(hash(1000)).toBe(expected);
});
