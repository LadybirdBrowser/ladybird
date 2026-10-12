test("compiled bitwise operations take booleans as 0 and 1", () => {
    const heap = new Float32Array([1, 2, 3, 4, 9, 1, 0, 7]);
    const combine = new Function(
        "heap",
        "i",
        `return [
            (heap[i] < heap[i + 1]) & (heap[i + 2] < heap[i + 3]) & 1,
            ((heap[i] < 2) | (heap[i] > 5)) ^ true,
            (heap[i] < heap[i + 1]) << 3,
            (heap[i] > heap[i + 1]) >>> 0,
            (heap[i] < heap[i + 2]) | 0,
            ~(heap[i] > heap[i + 3]),
        ];`
    );
    const expected = i => [
        (heap[i] < heap[i + 1]) & (heap[i + 2] < heap[i + 3]) & 1,
        ((heap[i] < 2) | (heap[i] > 5)) ^ true,
        (heap[i] < heap[i + 1]) << 3,
        (heap[i] > heap[i + 1]) >>> 0,
        (heap[i] < heap[i + 2]) | 0,
        ~(heap[i] > heap[i + 3]),
    ];
    jit.prepare(combine);
    for (let i = 0; i < 10; ++i) combine(heap, i & 3);
    expect(jit.compile(combine)).toBe(jit.enabled);
    for (let i = 0; i < 5; ++i) expect(combine(heap, i)).toEqual(expected(i));
    expect(jit.exitCount(combine)).toBe(0);
});
