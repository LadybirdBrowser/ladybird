const NUMERIC_TYPED_ARRAYS = [
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

function secondValueFor(T) {
    if (T === Float16Array || T === Float32Array || T === Float64Array) return 2.5;
    if (T === BigUint64Array || T === BigInt64Array) return 23n;
    return 23;
}

function replacementValueFor(T) {
    if (T === Float16Array || T === Float32Array || T === Float64Array) return 3.5;
    if (T === BigUint64Array || T === BigInt64Array) return 31n;
    return 31;
}

function testTypes(callback) {
    NUMERIC_TYPED_ARRAYS.forEach(callback);
    BIGINT_TYPED_ARRAYS.forEach(callback);
}

test("direct indexed access stays coherent for multiple fixed views over one buffer", () => {
    testTypes(T => {
        const buffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 4);
        const wholeView = new T(buffer);
        const offsetView = new T(buffer, T.BYTES_PER_ELEMENT, 2);
        const initialValue = initialValueFor(T);
        const secondValue = secondValueFor(T);

        wholeView[1] = initialValue;
        expect(offsetView[0]).toBe(initialValue);
        expect(Reflect.get(offsetView, 0)).toBe(initialValue);

        offsetView[1] = secondValue;
        expect(wholeView[2]).toBe(secondValue);
        expect(Reflect.get(wholeView, 2)).toBe(secondValue);
    });
});

test("detach invalidates every cached fixed view over the buffer", () => {
    testTypes(T => {
        const buffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 4);
        const wholeView = new T(buffer);
        const offsetView = new T(buffer, T.BYTES_PER_ELEMENT, 2);
        const initialValue = initialValueFor(T);
        const secondValue = secondValueFor(T);

        wholeView[0] = initialValue;
        offsetView[0] = secondValue;
        expect(wholeView[1]).toBe(secondValue);

        detachArrayBuffer(buffer);

        expect(buffer.detached).toBeTrue();
        expect(wholeView.length).toBe(0);
        expect(offsetView.length).toBe(0);
        expect(wholeView[0]).toBeUndefined();
        expect(offsetView[0]).toBeUndefined();
        expect(Reflect.get(wholeView, 0)).toBeUndefined();
        expect(Reflect.get(offsetView, 0)).toBeUndefined();

        wholeView[0] = replacementValueFor(T);
        offsetView[0] = replacementValueFor(T);

        expect(wholeView[0]).toBeUndefined();
        expect(offsetView[0]).toBeUndefined();
        expect(Object.hasOwn(wholeView, "0")).toBeFalse();
        expect(Object.hasOwn(offsetView, "0")).toBeFalse();
        expect(Object.getOwnPropertyDescriptor(wholeView, "0")).toBeUndefined();
        expect(Object.getOwnPropertyDescriptor(offsetView, "0")).toBeUndefined();
    });
});

test("ArrayBuffer.prototype.transfer invalidates old cached views", () => {
    testTypes(T => {
        const buffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 3);
        const oldWholeView = new T(buffer);
        const oldOffsetView = new T(buffer, T.BYTES_PER_ELEMENT, 1);
        const initialValue = initialValueFor(T);
        const secondValue = secondValueFor(T);
        const replacementValue = replacementValueFor(T);

        oldWholeView[0] = initialValue;
        oldOffsetView[0] = secondValue;

        const newBuffer = buffer.transfer();
        const newView = new T(newBuffer);

        expect(buffer.detached).toBeTrue();
        expect(newBuffer.detached).toBeFalse();
        expect(oldWholeView.length).toBe(0);
        expect(oldOffsetView.length).toBe(0);
        expect(oldWholeView[0]).toBeUndefined();
        expect(oldOffsetView[0]).toBeUndefined();
        expect(newView[0]).toBe(initialValue);
        expect(newView[1]).toBe(secondValue);

        oldWholeView[0] = replacementValue;
        oldOffsetView[0] = replacementValue;

        expect(newView[0]).toBe(initialValue);
        expect(newView[1]).toBe(secondValue);
    });
});

test("ArrayBuffer.prototype.transfer truncation invalidates offset cached views", () => {
    testTypes(T => {
        const buffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 4);
        const oldOffsetView = new T(buffer, T.BYTES_PER_ELEMENT * 2, 2);
        const initialValue = initialValueFor(T);
        const secondValue = secondValueFor(T);

        oldOffsetView[0] = initialValue;
        oldOffsetView[1] = secondValue;

        const newBuffer = buffer.transfer(T.BYTES_PER_ELEMENT * 3);
        const newView = new T(newBuffer);

        expect(buffer.detached).toBeTrue();
        expect(oldOffsetView.length).toBe(0);
        expect(oldOffsetView[0]).toBeUndefined();
        expect(newView.length).toBe(3);
        expect(newView[2]).toBe(initialValue);
    });
});

test("ArrayBuffer.prototype.transferToFixedLength invalidates old cached views", () => {
    testTypes(T => {
        const buffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 3);
        const oldView = new T(buffer);
        const initialValue = initialValueFor(T);

        oldView[2] = initialValue;

        const newBuffer = buffer.transferToFixedLength();
        const newView = new T(newBuffer);

        expect(buffer.detached).toBeTrue();
        expect(newBuffer.detached).toBeFalse();
        expect(oldView.length).toBe(0);
        expect(oldView[2]).toBeUndefined();
        expect(newView.length).toBe(3);
        expect(newView[2]).toBe(initialValue);

        detachArrayBuffer(newBuffer);

        expect(newBuffer.detached).toBeTrue();
        expect(newView.length).toBe(0);
        expect(newView[0]).toBeUndefined();
    });
});

test("ArrayBuffer.prototype.slice creates an independently cached fixed buffer", () => {
    testTypes(T => {
        const buffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 4);
        const originalView = new T(buffer);
        const initialValue = initialValueFor(T);
        const secondValue = secondValueFor(T);

        originalView[1] = initialValue;
        originalView[2] = secondValue;

        const slicedBuffer = buffer.slice(T.BYTES_PER_ELEMENT, T.BYTES_PER_ELEMENT * 3);
        const slicedView = new T(slicedBuffer);

        expect(slicedView.length).toBe(2);
        expect(slicedView[0]).toBe(initialValue);
        expect(slicedView[1]).toBe(secondValue);

        detachArrayBuffer(buffer);

        expect(buffer.detached).toBeTrue();
        expect(originalView.length).toBe(0);
        expect(originalView[1]).toBeUndefined();
        expect(slicedBuffer.detached).toBeFalse();
        expect(slicedView[0]).toBe(initialValue);
        expect(slicedView[1]).toBe(secondValue);
    });
});

test("fixed SharedArrayBuffer cached views stay coherent", () => {
    NUMERIC_TYPED_ARRAYS.forEach(T => {
        const buffer = new SharedArrayBuffer(T.BYTES_PER_ELEMENT * 4);
        const wholeView = new T(buffer);
        const offsetView = new T(buffer, T.BYTES_PER_ELEMENT, 2);
        const initialValue = initialValueFor(T);
        const secondValue = secondValueFor(T);

        wholeView[1] = initialValue;
        expect(offsetView[0]).toBe(initialValue);

        offsetView[1] = secondValue;
        expect(wholeView[2]).toBe(secondValue);
        expect(Reflect.get(wholeView, 2)).toBe(secondValue);
    });
});
