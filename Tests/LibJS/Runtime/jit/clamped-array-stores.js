test("compiled clamped stores round ties to even and saturate", () => {
    const store = new Function("array", "index", "value", "array[index] = value; return array[index];");
    const array = new Uint8ClampedArray(2);
    jit.prepare(store);
    for (let i = 0; i < 10; ++i) store(array, 1, 1.25);
    expect(jit.compile(store)).toBe(jit.enabled);
    for (const [value, expected] of [
        [-Infinity, 0],
        [-1e300, 0],
        [-0.5, 0],
        [-0, 0],
        [0, 0],
        [NaN, 0],
        [Number.MIN_VALUE, 0],
        [0.49999999999999994, 0],
        [0.5, 0],
        [0.5000000000000001, 1],
        [1.4999999999999998, 1],
        [1.5, 2],
        [1.5000000000000002, 2],
        [2.5, 2],
        [3.5, 4],
        [254.49999999999997, 254],
        [254.5, 254],
        [254.50000000000003, 255],
        [255, 255],
        [255.5, 255],
        [1e300, 255],
        [Infinity, 255],
    ]) {
        expect(store(array, 1, value)).toBe(expected);
        expect(array[0]).toBe(0);
    }
    expect(jit.exitCount(store)).toBe(0);
});

test("clamped stores preserve coercion and invalid indices", () => {
    const store = new Function("array", "index", "value", "array[index] = value; return array[index];");
    const array = new Uint8ClampedArray(2);
    jit.prepare(store);
    for (let i = 0; i < 10; ++i) store(array, 0, 2.25);
    jit.compile(store);
    let calls = 0;
    const value = {
        valueOf() {
            ++calls;
            return 3.5;
        },
    };
    expect(store(array, 0, value)).toBe(4);
    expect(calls).toBe(1);
    expect(store(array, 0, "2.5")).toBe(2);
    expect(store(array, 0, undefined)).toBe(0);
    expect(() => store(array, 0, 1n)).toThrow(TypeError);
    expect(store(array, -1, 8.5)).toBeUndefined();
    expect(store(array, 2, 8.5)).toBeUndefined();
    expect(store(array, 0.5, 8.5)).toBeUndefined();
});

test("compiled clamped stores handle resizing and detaching buffers", () => {
    const compileStore = array => {
        const store = new Function("array", "index", "value", "array[index] = value; return array[index];");
        jit.prepare(store);
        for (let i = 0; i < 10; ++i) store(array, 0, 2.25);
        expect(jit.compile(store)).toBe(jit.enabled);
        return store;
    };
    const buffer = new ArrayBuffer(2, { maxByteLength: 8 });
    const resizable = new Uint8ClampedArray(buffer);
    const store = compileStore(resizable);
    expect(store(resizable, 1, 2.5)).toBe(2);
    buffer.resize(1);
    expect(store(resizable, 1, 3.5)).toBeUndefined();
    buffer.resize(4);
    expect(store(resizable, 3, 3.5)).toBe(4);
    const array = new Uint8ClampedArray(2);
    const detachedStore = compileStore(array);
    detachArrayBuffer(array.buffer);
    expect(detachedStore(array, 0, 5.5)).toBeUndefined();
});
