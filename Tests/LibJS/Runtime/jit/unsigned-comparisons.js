test("compiled comparisons of uint32 values compare unsigned", () => {
    const compare = new Function(
        "a",
        "b",
        `a = a | 0;
        b = b | 0;
        return [a >>> 0 < b >>> 0, a >>> 0 <= b >>> 0, a >>> 0 > b >>> 0, a >>> 0 >= b >>> 0, a >>> 0 === b >>> 0,
            ((a - 8) | 0) >>> 0 < 20, (a >>> 0) > 4294967294, (a >>> 0) < 0.5];`
    );
    const expected = (a, b) => {
        const x = a >>> 0;
        const y = b >>> 0;
        return [x < y, x <= y, x > y, x >= y, x === y, ((a - 8) | 0) >>> 0 < 20, x > 4294967294, x < 0.5];
    };
    jit.prepare(compare);
    for (let i = 0; i < 10; ++i) {
        compare(i, -3);
        compare(-i, 3);
    }
    expect(jit.compile(compare)).toBe(jit.enabled);
    const values = [0, 1, 7, 8, 27, 28, -1, -2, 0x7fffffff, -0x80000000, 0x40000000];
    for (const a of values) {
        for (const b of values) expect(compare(a, b)).toEqual(expected(a, b));
    }
    expect(jit.exitCount(compare)).toBe(0);
});
