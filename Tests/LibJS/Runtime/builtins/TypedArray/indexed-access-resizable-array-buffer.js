const TYPED_ARRAYS = [
    Uint8Array,
    Uint8ClampedArray,
    Uint16Array,
    Uint32Array,
    Int8Array,
    Int16Array,
    Int32Array,
    Float16Array,
    Float32Array,
    Float64Array,
];

const BIGINT_TYPED_ARRAYS = [BigUint64Array, BigInt64Array];

function initialValueFor(T) {
    if (T === Float16Array || T === Float32Array || T === Float64Array) return 1.5;
    if (T === BigUint64Array || T === BigInt64Array) return 17n;
    return 17;
}

function replacementValueFor(T) {
    if (T === Float16Array || T === Float32Array || T === Float64Array) return 2.5;
    if (T === BigUint64Array || T === BigInt64Array) return 23n;
    return 23;
}

function zeroValueFor(T) {
    if (T === BigUint64Array || T === BigInt64Array) return 0n;
    return 0;
}

function testTypes(callback) {
    TYPED_ARRAYS.forEach(callback);
    BIGINT_TYPED_ARRAYS.forEach(callback);
}

test("direct indexed access becomes undefined when a fixed-length view goes out of bounds", () => {
    testTypes(T => {
        let arrayBuffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 2, {
            maxByteLength: T.BYTES_PER_ELEMENT * 4,
        });
        let typedArray = new T(arrayBuffer, T.BYTES_PER_ELEMENT, 1);

        let initialValue = initialValueFor(T);
        typedArray[0] = initialValue;
        expect(typedArray[0]).toBe(initialValue);
        expect(Reflect.get(typedArray, 0)).toBe(initialValue);

        arrayBuffer.resize(T.BYTES_PER_ELEMENT);

        expect(typedArray.length).toBe(0);
        expect(typedArray.byteOffset).toBe(0);
        expect(typedArray[0]).toBeUndefined();
        expect(Reflect.get(typedArray, 0)).toBeUndefined();
        expect(Object.hasOwn(typedArray, "0")).toBeFalse();
        expect(Object.getOwnPropertyDescriptor(typedArray, "0")).toBeUndefined();
    });
});

test("direct indexed stores remain no-ops while a fixed-length view is out of bounds", () => {
    testTypes(T => {
        let arrayBuffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 2, {
            maxByteLength: T.BYTES_PER_ELEMENT * 4,
        });
        let typedArray = new T(arrayBuffer, T.BYTES_PER_ELEMENT, 1);

        typedArray[0] = initialValueFor(T);
        arrayBuffer.resize(T.BYTES_PER_ELEMENT);

        typedArray[0] = replacementValueFor(T);

        expect(typedArray[0]).toBeUndefined();
        expect(Reflect.get(typedArray, 0)).toBeUndefined();
        expect(Object.hasOwn(typedArray, "0")).toBeFalse();
        expect(Object.getOwnPropertyDescriptor(typedArray, "0")).toBeUndefined();
    });
});

test("direct indexed access observes zero-filled bytes after regrowing the buffer", () => {
    testTypes(T => {
        let arrayBuffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 2, {
            maxByteLength: T.BYTES_PER_ELEMENT * 4,
        });
        let typedArray = new T(arrayBuffer, T.BYTES_PER_ELEMENT, 1);

        typedArray[0] = initialValueFor(T);
        arrayBuffer.resize(T.BYTES_PER_ELEMENT);
        arrayBuffer.resize(T.BYTES_PER_ELEMENT * 2);

        expect(typedArray.length).toBe(1);
        expect(typedArray.byteOffset).toBe(T.BYTES_PER_ELEMENT);
        expect(typedArray[0]).toBe(zeroValueFor(T));
        expect(Reflect.get(typedArray, 0)).toBe(zeroValueFor(T));

        typedArray[0] = replacementValueFor(T);
        expect(typedArray[0]).toBe(replacementValueFor(T));
        expect(Reflect.get(typedArray, 0)).toBe(replacementValueFor(T));
    });
});

test("direct indexed access stays invalid after the backing buffer is detached", () => {
    testTypes(T => {
        let typedArray = new T(1);
        typedArray[0] = initialValueFor(T);

        detachArrayBuffer(typedArray.buffer);

        expect(typedArray.length).toBe(0);
        expect(typedArray[0]).toBeUndefined();
        expect(Reflect.get(typedArray, 0)).toBeUndefined();

        typedArray[0] = replacementValueFor(T);

        expect(typedArray[0]).toBeUndefined();
        expect(Reflect.get(typedArray, 0)).toBeUndefined();
        expect(Object.hasOwn(typedArray, "0")).toBeFalse();
        expect(Object.getOwnPropertyDescriptor(typedArray, "0")).toBeUndefined();
    });
});
