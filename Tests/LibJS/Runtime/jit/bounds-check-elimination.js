function compiled(f, ...warmupArguments) {
    jit.prepare(f);
    for (let i = 0; i < 10; ++i) f(...warmupArguments);
    expect(jit.compile(f)).toBe(jit.enabled);
    return f;
}

test("counted loops stay within their bounds without bounds checks", () => {
    const fill = compiled(
        new Function(
            "heap",
            "start",
            "step",
            "end",
            `let i = start | 0, count = 0;
            do {
                heap[i] = (heap[i] | 0) + 1;
                count = (count + 1) | 0;
                i = (i + step) | 0;
            } while ((i | 0) != (end | 0) && count < 100);
            return count;`
        ),
        new Int32Array(16),
        0,
        1,
        16
    );
    const heap = new Int32Array(16);
    expect(fill(heap, 0, 1, 16)).toBe(16);
    expect(Array.from(heap)).toEqual(new Array(16).fill(1));
    // NB: A step that jumps over the bound runs past the array.
    const small = new Int32Array(4);
    expect(fill(small, 1, 2, 4)).toBe(100);
    expect(Array.from(small)).toEqual([0, 1, 0, 1]);
});

test("loops with constant bounds handle every start", () => {
    const sum = compiled(
        new Function(
            "heap",
            "start",
            `let i = start | 0, total = 0;
            do {
                total = (total + (heap[i] | 0)) | 0;
                i = (i + 1) | 0;
            } while ((i | 0) != 8);
            return total;`
        ),
        new Int32Array([1, 2, 3, 4, 5, 6, 7, 8]),
        0
    );
    const heap = new Int32Array([1, 2, 3, 4, 5, 6, 7, 8]);
    expect(sum(heap, 0)).toBe(36);
    expect(sum(heap, 5)).toBe(21);
    expect(sum(heap, -2)).toBe(36);
    expect(sum(heap, 7)).toBe(8);
});

test("loops entered in the middle by on-stack replacement keep their checks", () => {
    const heap = new Int32Array(64);
    let total = 0;
    for (let round = 0; round < 3; ++round) {
        let i = round === 2 ? 60 : 0;
        do {
            heap[i] = i;
            total = (total + heap[i]) | 0;
            i = (i + 1) | 0;
        } while ((i | 0) != 64);
    }
    expect(total).toBe(2016 + 2016 + (60 + 61 + 62 + 63));
});

test("scaled indices keep their arithmetic where it may overflow", () => {
    const body = `let i = start | 0, n = 0, total = 0;
        do {
            total = (total + (heap[(8 + (i << 2)) >> 2] | 0)) | 0;
            i = (i + 1) | 0;
            n = (n + 1) | 0;
        } while ((n | 0) != 4);
        return total;`;
    const read = compiled(new Function("heap", "start", body), new Int32Array(6), 0);
    const expected = new Function("heap", "start", body);
    const heap = new Int32Array([1, 2, 3, 4, 5, 6]);
    // NB: `i << 2` overflows for some of these starts, and wraps around.
    for (const start of [0, 1, 0x3ffffffe, 0x3fffffff, -0x40000000, -3, 0x7ffffffe])
        expect(read(heap, start)).toBe(expected(heap, start));
});
