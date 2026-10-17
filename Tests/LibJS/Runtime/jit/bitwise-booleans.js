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

test("compiled bitwise operations take operands that are sometimes booleans", () => {
    // NB: Like compiled asm.js code, where a local holds an int32 or a comparison.
    const combine = new Function(
        "a",
        "b",
        `let mixed = 0;
        mixed = a & 1 ? a >= b : mixed;
        return [mixed | (a < b), (b & 3) ^ mixed, mixed & (a > 2), ~mixed, mixed | 0, mixed >>> 0];`
    );
    const expected = (a, b) => {
        let mixed = 0;
        mixed = a & 1 ? a >= b : mixed;
        return [mixed | (a < b), (b & 3) ^ mixed, mixed & (a > 2), ~mixed, mixed | 0, mixed >>> 0];
    };
    jit.prepare(combine);
    for (let i = 0; i < 10; ++i) combine(i, 5);
    expect(jit.compile(combine)).toBe(jit.enabled);
    for (let i = 0; i < 10; ++i) expect(combine(i, 5)).toEqual(expected(i, 5));
    expect(jit.exitCount(combine)).toBe(0);

    // Other values still convert like ToInt32 does.
    for (const value of [1.5, -2.75, 2 ** 32 + 3, undefined, null, "7", {}]) {
        const bitwise = new Function("v", "w", "return [v | w, v & 7, v ^ w, ~v, v | 0];");
        jit.prepare(bitwise);
        for (let i = 0; i < 10; ++i) bitwise(i & 1 ? i > 4 : i, i < 3);
        jit.compile(bitwise);
        expect(bitwise(value, true)).toEqual([value | true, value & 7, value ^ true, ~value, value | 0]);
    }
});
