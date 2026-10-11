const storageConstructors = [
    Int8Array,
    Uint8Array,
    Uint8ClampedArray,
    Int16Array,
    Uint16Array,
    Int32Array,
    Uint32Array,
    Float32Array,
    Float64Array,
];

function compileStorage(f, args) {
    jit.prepare(f);
    for (let i = 0; i < 10; ++i) f(...args);
    expect(jit.compile(f)).toBe(jit.enabled);
    return f;
}

test("constant and dynamic indices use the storage of offset views", () => {
    for (const Constructor of storageConstructors) {
        const parent = new Constructor(8);
        parent.fill(99);
        const view = parent.subarray(2, 6);
        const access = compileStorage(
            new Function(
                "array",
                "index",
                "array[0] = 11; array[2] = 22; array[index] = 33; return array[0] + array[2] + array[index];"
            ),
            [view, 1]
        );
        expect(access(view, 1)).toBe(66);
        expect(Array.from(parent)).toEqual([99, 99, 11, 33, 22, 99, 99, 99]);
        const other = new Constructor([0, 0, 0, 0]);
        expect(access(other, 3)).toBe(66);
        expect(Array.from(other)).toEqual([11, 0, 22, 33]);
        expect(jit.exitCount(access)).toBe(0);
    }
});

test("constant negative and large indices stay out of bounds", () => {
    for (const Constructor of storageConstructors) {
        for (const index of [-1, -2147483648, 2147483647, 4294967295, 3]) {
            const array = new Constructor([1, 2, 3]);
            const access = compileStorage(new Function("array", `array[${index}] = 77; return array[${index}];`), [
                array,
            ]);
            expect(access(array)).toBeUndefined();
            expect(Array.from(array)).toEqual([1, 2, 3]);
        }
    }
});

test("calls that detach a buffer invalidate reused typed-array storage", () => {
    for (const Constructor of storageConstructors) {
        const array = new Constructor([1, 2, 3]);
        const access = compileStorage(
            new Function(
                "array",
                "callback",
                "array[0] = 11; const before = array[0]; callback(); array[1] = 22; return [before, array[0], array[1]];"
            ),
            [array, () => {}]
        );
        const detach = () => detachArrayBuffer(array.buffer);
        jit.neverCompile(detach);
        expect(access(array, detach)).toEqual([11, undefined, undefined]);
        expect(array.length).toBe(0);
    }
});

test("storage loads in loops observe detachment between iterations", () => {
    const array = new Int32Array([42]);
    const access = compileStorage(
        new Function(
            "array",
            "callback",
            "const values = []; for (let i = 0; i < 4; ++i) { values.push(array[0]); callback(i); array[0] = 42; } return values;"
        ),
        [array, () => {}]
    );
    const detach = i => {
        if (i === 1) detachArrayBuffer(array.buffer);
    };
    jit.neverCompile(detach);
    expect(access(array, detach)).toEqual([42, 42, undefined, undefined]);
});

test("resizable buffers are checked again after a call", () => {
    for (const Constructor of storageConstructors) {
        const size = Constructor.BYTES_PER_ELEMENT;
        const buffer = new ArrayBuffer(4 * size, { maxByteLength: 8 * size });
        const array = new Constructor(buffer);
        const access = compileStorage(
            new Function(
                "array",
                "callback",
                "array[2] = 33; const before = array[2]; callback(); array[2] = 44; return [before, array[2]];"
            ),
            [array, () => {}]
        );
        expect(access(array, () => buffer.resize(size))).toEqual([33, undefined]);
        expect(access(array, () => buffer.resize(8 * size))).toEqual([undefined, 44]);
    }
});

test("constant indices also work with packed and holey arrays", () => {
    for (const array of [
        [1, 2, 3],
        [1, , 3],
    ]) {
        const access = compileStorage(
            new Function(
                "array",
                "array[0] = 11; array[2] = 22; return [array[0], array[1], array[2], array[-1], array[2147483647]];"
            ),
            [array]
        );
        expect(access(array)).toEqual([11, array[1], 22, undefined, undefined]);
    }
});
