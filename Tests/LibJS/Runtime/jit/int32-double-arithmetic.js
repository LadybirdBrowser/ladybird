test("compiled ToInt32 of sums of int32 values that saw overflows wraps", () => {
    const heap = new Int32Array(4);
    const store = new Function(
        "heap",
        "a",
        "b",
        "c",
        `a = a | 0; b = b | 0; c = c | 0;
        heap[0] = a + b;
        heap[1] = a - b + c;
        heap[2] = a + b + 0.5;
        heap[3] = a + b + 1;
        return heap[0] + heap[1] * 1000 + heap[3] + heap[2] * 7;`
    );
    const expected = (a, b, c) => {
        const results = [a + b, a - b + c, a + b + 0.5, a + b + 1].map(value => value | 0);
        return [results[0] + results[1] * 1000 + results[3] + results[2] * 7, ...results];
    };
    jit.prepare(store);
    for (let i = 0; i < 10; ++i) {
        store(heap, i, 3, 1);
        store(heap, 0x7fffffff, 0x7fffffff, 5);
    }
    expect(jit.compile(store)).toBe(jit.enabled);
    for (const [a, b, c] of [
        [1, 2, 3],
        [0x7fffffff, 0x7fffffff, 5],
        [-0x80000000, 1, -0x80000000],
        [0x7fffffff, -0x80000000, 0x7fffffff],
    ])
        expect([store(heap, a, b, c), ...heap]).toEqual(expected(a, b, c));
});
