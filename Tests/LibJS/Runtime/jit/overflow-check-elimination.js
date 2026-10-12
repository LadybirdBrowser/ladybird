function compiled(f, ...warmupArguments) {
    jit.prepare(f);
    for (let i = 0; i < 10; ++i) f(...warmupArguments);
    expect(jit.compile(f)).toBe(jit.enabled);
    return f;
}

test("products and sums of masked values at their limits", () => {
    const multiply = compiled(
        new Function("x", "y", "return (x & 0x3fff) * (y & 0x3fff) + ((x >> 14) - (y >> 14));"),
        1000,
        7000
    );
    expect(multiply(0x3fff, 0x3fff)).toBe(0x3fff * 0x3fff);
    expect(multiply(-1, -1)).toBe(0x3fff * 0x3fff);
    expect(multiply(2147483647, -2147483648)).toBe(0x3fff * 0 + (131071 - -131072));
    expect(multiply(-2147483648, 2147483647)).toBe(0 + (-131072 - 131071));
});

test("products that may be -0 or overflow keep their results", () => {
    const multiply = compiled(new Function("x", "y", "return (x >> 14) * (y & 0x3fff);"), 1000, 7000);
    expect(Object.is(multiply(-16384, 0), -0)).toBeTrue();
    expect(multiply(-2147483648, 0x3fff)).toBe(-131072 * 0x3fff);
    const wide = compiled(new Function("x", "y", "return x * y;"), 3, 4);
    expect(wide(65536, 65536)).toBe(4294967296);
    expect(Object.is(wide(-1, 0), -0)).toBeTrue();
});

test("indices incremented after their accesses", () => {
    const sum = compiled(
        new Function(
            "a",
            "n",
            `let i = 0, s = 0;
            while (n-- > 0) {
                s = (s + a[i]) | 0;
                i = i + 1;
            }
            return s + i;`
        ),
        [1, 2, 3],
        3
    );
    expect(sum([1, 2, 3, 4], 4)).toBe(14);
    // NB: Past the end, the sum adds undefined, which | 0 makes 0.
    expect(sum([1, 2], 3)).toBe(3);
});

test("indices incremented after accesses of a typed array and an array", () => {
    const sum = compiled(
        new Function("ta", "a", "i", "const j = i & 0x7fffffff; return ta[j] + a[j] + (j + 1);"),
        new Int32Array(4),
        [1, 2, 3, 4],
        2
    );
    expect(sum(new Int32Array([5, 6, 7]), [1, 2, 3], 2)).toBe(7 + 3 + 3);
    expect(sum(new Int32Array([5]), [1, 2, 3], -1)).toBeNaN();
});

test("indices incremented after accesses of a fixed length typed array", () => {
    const table = new Int32Array(256);
    table[255] = 7;
    const lookup = compiled(new Function("table", "return (i) => table[i] + (i + 1);")(table), 3);
    expect(lookup(255)).toBe(7 + 256);
    expect(lookup(2147483647)).toBeNaN();
    expect(lookup(-1)).toBeNaN();
});

test("products of unsigned right shifts of any int32 at their limits", () => {
    const mix = compiled(new Function("h", "x", "return (h >>> 24) * 3 + (x & 0xff) + (h >>> 1) * 1;"), 5, 6);
    expect(mix(-1, -1)).toBe(255 * 3 + 255 + 2147483647);
    expect(mix(-2147483648, 0)).toBe(128 * 3 + 1073741824);
    expect(mix(2147483647, 255)).toBe(127 * 3 + 255 + 1073741823);
});
