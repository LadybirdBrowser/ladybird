const numericArrayConstructors = [
    Int8Array,
    Uint8Array,
    Int16Array,
    Uint16Array,
    Int32Array,
    Uint32Array,
    Float32Array,
    Float64Array,
];

function compileNumericStore(array) {
    const store = new Function("array", "index", "value", "array[index] = value; return array[index];");
    jit.prepare(store);
    for (let i = 0; i < 10; ++i) store(array, 1, 1.25);
    expect(jit.compile(store)).toBe(jit.enabled);
    return store;
}

test("compiled numeric typed-array stores convert all numbers without exits", () => {
    const values = [
        -Infinity,
        -1e300,
        -4294967297.75,
        -2147483649.5,
        -65537.25,
        -257.75,
        -1.75,
        -0,
        0,
        Number.MIN_VALUE,
        0.5,
        1.75,
        255.75,
        65535.75,
        2147483647.75,
        2147483648,
        4294967295,
        4294967297.75,
        1e300,
        Infinity,
        NaN,
    ];
    for (const Constructor of numericArrayConstructors) {
        const array = new Constructor(3);
        const referenceArray = new Constructor(3);
        const store = compileNumericStore(array);
        const reference = new Function("array", "index", "value", "array[index] = value; return array[index];");
        jit.neverCompile(reference);
        for (const value of values) {
            expect(store(array, 1, value)).toBe(reference(referenceArray, 1, value));
            expect(array[0]).toBe(0);
            expect(array[2]).toBe(0);
        }
        expect(jit.exitCount(store)).toBe(0);
    }
});

test("numeric stores preserve coercion and changes to the elements kind", () => {
    for (const Constructor of numericArrayConstructors) {
        const array = new Constructor(3);
        const store = compileNumericStore(array);
        let calls = 0;
        const value = {
            valueOf() {
                ++calls;
                return -1.75;
            },
        };
        const expected = new Constructor([-1.75])[0];
        expect(store(array, 1, value)).toBe(expected);
        expect(calls).toBe(1);
        expect(() => store(array, 1, 1n)).toThrow(TypeError);
        expect(store(array, 1, "-1.75")).toBe(expected);
        const other = new Uint8ClampedArray(3);
        const freshStore = compileNumericStore(array);
        expect(freshStore(other, 1, 3.5)).toBe(4);
        expect(other[0]).toBe(0);
        expect(other[2]).toBe(0);
    }
});

test("numeric conversion does not change the value of the assignment", () => {
    for (const Constructor of numericArrayConstructors) {
        const array = new Constructor(3);
        const assign = new Function("array", "value", "return [array[1] = value, array[1]];");
        jit.prepare(assign);
        for (let i = 0; i < 10; ++i) assign(array, 1.25);
        expect(jit.compile(assign)).toBe(jit.enabled);
        for (const value of [-1.75, -0, 4294967295, Infinity, NaN]) {
            expect(assign(array, value)).toEqual([value, new Constructor([value])[0]]);
        }
        expect(jit.exitCount(assign)).toBe(0);
    }
});

test("numeric stores check indices and changed buffers", () => {
    for (const Constructor of numericArrayConstructors) {
        const array = new Constructor(3);
        for (const index of [-1, 3, 0.5, "-0", NaN]) {
            const store = compileNumericStore(array);
            expect(store(array, index, 4294967295)).toBeUndefined();
            expect(array[0]).toBe(0);
            expect(array[2]).toBe(0);
        }
        const buffer = new ArrayBuffer(3 * Constructor.BYTES_PER_ELEMENT, {
            maxByteLength: 5 * Constructor.BYTES_PER_ELEMENT,
        });
        const resizable = new Constructor(buffer);
        const store = compileNumericStore(resizable);
        buffer.resize(Constructor.BYTES_PER_ELEMENT);
        expect(store(resizable, 1, 1.75)).toBeUndefined();
        buffer.resize(5 * Constructor.BYTES_PER_ELEMENT);
        expect(store(resizable, 4, 1.75)).toBe(new Constructor([1.75])[0]);
        const detached = new Constructor(3);
        const detachedStore = compileNumericStore(detached);
        detachArrayBuffer(detached.buffer);
        expect(detachedStore(detached, 1, 1.75)).toBeUndefined();
        const coercionArray = new Constructor(3);
        const coercionStore = compileNumericStore(coercionArray);
        let calls = 0;
        expect(
            coercionStore(coercionArray, 1, {
                valueOf() {
                    ++calls;
                    detachArrayBuffer(coercionArray.buffer);
                    return 1.75;
                },
            })
        ).toBeUndefined();
        expect(calls).toBe(1);
    }
});

test("numeric store guards restore a later loop iteration before coercion", () => {
    const copy = new Function(
        "output",
        "input",
        "for (let i = 0; i < input.length; ++i) output[i] = input[i]; return output;"
    );
    const values = Array.from({ length: 20 }, (_, i) => 4294967200 + i);
    const output = new Uint32Array(20);
    jit.prepare(copy);
    for (let i = 0; i < 10; ++i) copy(output, values);
    expect(jit.compile(copy)).toBe(jit.enabled);
    let calls = 0;
    output.fill(0);
    values[13] = {
        valueOf() {
            ++calls;
            return -1.75;
        },
    };
    copy(output, values);
    expect(calls).toBe(1);
    for (let i = 0; i < output.length; ++i) expect(output[i]).toBe(i === 13 ? 4294967295 : 4294967200 + i);
});

test("integer values retain their bits through signed and unsigned numeric stores", () => {
    const values = [-2147483648, -65537, -257, -1, 0, 1, 255, 65535, 2147483647];
    for (const Constructor of numericArrayConstructors) {
        for (const unsigned of [false, true]) {
            const store = new Function(
                "array",
                "value",
                unsigned ? "array[0] = value >>> 0; return array[0];" : "array[0] = value | 0; return array[0];"
            );
            const array = new Constructor(1);
            jit.prepare(store);
            for (const value of values) store(array, value);
            expect(jit.compile(store)).toBe(jit.enabled);
            for (const value of values) {
                expect(store(array, value)).toBe(new Constructor([unsigned ? value >>> 0 : value | 0])[0]);
            }
            expect(jit.exitCount(store)).toBe(0);
        }
    }
});
