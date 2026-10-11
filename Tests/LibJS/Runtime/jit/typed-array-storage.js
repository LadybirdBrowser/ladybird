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
    let shouldDetach = false;
    const detach = i => {
        if (shouldDetach && i === 1) detachArrayBuffer(array.buffer);
    };
    jit.neverCompile(detach);
    const access = compileStorage(
        new Function(
            "array",
            "callback",
            "const values = []; for (let i = 0; i < 4; ++i) { values.push(array[0]); callback(i); array[0] = 42; } return values;"
        ),
        [array, detach]
    );
    shouldDetach = true;
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

test("captured fixed lengths keep a detachment guard for constant indices", () => {
    for (const Constructor of storageConstructors) {
        const parent = new Constructor([99, 1, 2, 3, 99]);
        const array = parent.subarray(1, 4);
        let shouldDetach = false;
        const detach = () => {
            if (shouldDetach) detachArrayBuffer(array.buffer);
        };
        jit.neverCompile(detach);
        const factory = new Function(
            "array",
            "return function access(callback) { array[0] = 11; const before = array[0]; callback(); array[1] = 22; return [before, array[0], array[1]]; };"
        );
        const access = compileStorage(factory(array), [detach]);
        gc();
        expect(access(detach)).toEqual([11, 11, 22]);
        expect(parent[0]).toBe(99);
        expect(parent[4]).toBe(99);
        shouldDetach = true;
        expect(access(detach)).toEqual([11, undefined, undefined]);
        shouldDetach = false;
        expect(access(detach)).toEqual([undefined, undefined, undefined]);
    }
});

test("captured resizable buffers retain dynamic length checks", () => {
    for (const Constructor of storageConstructors) {
        const size = Constructor.BYTES_PER_ELEMENT;
        const buffer = new ArrayBuffer(4 * size, { maxByteLength: 8 * size });
        const array = new Constructor(buffer);
        let length = 4 * size;
        const resize = () => buffer.resize(length);
        jit.neverCompile(resize);
        const factory = new Function(
            "array",
            "return function access(callback) { array[2] = 33; const before = array[2]; callback(); array[2] = 44; return [before, array[2]]; };"
        );
        const access = compileStorage(factory(array), [resize]);
        length = size;
        expect(access(resize)).toEqual([33, undefined]);
        length = 8 * size;
        expect(access(resize)).toEqual([undefined, 44]);
    }
});

const conditionalGlobalStorage = new Uint8Array(16);
function sumConditionalGlobalStorage(count, limit) {
    let sum = 0;
    for (let i = 0; i < count; i++) if (i < limit) sum += conditionalGlobalStorage[0];
    return sum;
}

test("conditional constant-index loads of a global stay behind the storage check", () => {
    conditionalGlobalStorage[0] = 1;
    const sum = compileStorage(sumConditionalGlobalStorage, [100, 100]);
    expect(sum(100, 100)).toBe(100);
    conditionalGlobalStorage.buffer.transfer();
    expect(sum(3, 0)).toBe(0);
    expect(sum(3, 1)).toBeNaN();
});

test("conditional constant-index loads of a captured binding stay behind the storage check", () => {
    const factory = new Function(
        "return function sum(count, limit) { let storage = new Uint8Array(16); storage[0] = 1; " +
            "function inner(count, limit) { let sum = 0; for (let i = 0; i < count; i++) if (i < limit) sum += storage[0]; return sum; } " +
            "jit.prepare(inner); for (let i = 0; i < 10; ++i) inner(count, limit); jit.compile(inner); " +
            "const before = inner(count, limit); storage.buffer.transfer(); return [before, inner(3, 0), inner(3, 1)]; };"
    );
    const [before, skipped, loaded] = factory()(100, 100);
    expect(before).toBe(100);
    expect(skipped).toBe(0);
    expect(loaded).toBeNaN();
});

const detachedGlobalStorage = new Uint8Array([1, 2, 3, 4]);
function readDetachedGlobalStorage(index) {
    return detachedGlobalStorage[index];
}

test("keyed accesses compiled after out-of-bounds exits take the slow path once detached", () => {
    const capturedFactory = new Function("storage", "return function read(index) { return storage[index]; };");
    const captured = new Uint8Array([1, 2, 3, 4]);
    for (const [read, storage] of [
        [capturedFactory(captured), captured],
        [readDetachedGlobalStorage, detachedGlobalStorage],
    ]) {
        compileStorage(read, [2]);
        expect(read(2)).toBe(3);
        expect(read(10)).toBeUndefined();
        jit.discard(read);
        expect(jit.compile(read)).toBe(jit.enabled);
        expect(read(2)).toBe(3);
        expect(read(10)).toBeUndefined();
        storage.buffer.transfer();
        expect(read(2)).toBeUndefined();
        expect(read(0)).toBeUndefined();
        expect(jit.exitCount(read)).toBe(0);
    }
});
