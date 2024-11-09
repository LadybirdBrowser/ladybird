const TYPED_ARRAYS = [
    Uint8Array,
    Uint8ClampedArray,
    Uint16Array,
    Uint32Array,
    Int8Array,
    Int16Array,
    Int32Array,
    Float32Array,
    Float64Array,
];

const BIGINT_TYPED_ARRAYS = [BigUint64Array, BigInt64Array];

describe("errors", () => {
    test("ArrayBuffer out of bounds", () => {
        TYPED_ARRAYS.forEach(T => {
            let arrayBuffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 2, {
                maxByteLength: T.BYTES_PER_ELEMENT * 4,
            });

            let typedArray = new T(arrayBuffer, T.BYTES_PER_ELEMENT, 1);
            arrayBuffer.resize(T.BYTES_PER_ELEMENT);

            expect(() => {
                typedArray.entries();
            }).toThrowWithMessage(
                TypeError,
                "TypedArray contains a property which references a value at an index not contained within its buffer's bounds"
            );
        });
    });
});

test("length is 0", () => {
    TYPED_ARRAYS.forEach(T => {
        expect(T.prototype.entries).toHaveLength(0);
    });

    BIGINT_TYPED_ARRAYS.forEach(T => {
        expect(T.prototype.entries).toHaveLength(0);
    });
});

test("basic functionality", () => {
    TYPED_ARRAYS.forEach(T => {
        const a = new T([30, 40, 50]);
        const it = a.entries();
        expect(it.next()).toEqual({ value: [0, 30], done: false });
        expect(it.next()).toEqual({ value: [1, 40], done: false });
        expect(it.next()).toEqual({ value: [2, 50], done: false });
        expect(it.next()).toEqual({ value: undefined, done: true });
        expect(it.next()).toEqual({ value: undefined, done: true });
        expect(it.next()).toEqual({ value: undefined, done: true });
    });

    BIGINT_TYPED_ARRAYS.forEach(T => {
        const a = new T([30n, 40n, 50n]);
        const it = a.entries();
        expect(it.next()).toEqual({ value: [0, 30n], done: false });
        expect(it.next()).toEqual({ value: [1, 40n], done: false });
        expect(it.next()).toEqual({ value: [2, 50n], done: false });
        expect(it.next()).toEqual({ value: undefined, done: true });
        expect(it.next()).toEqual({ value: undefined, done: true });
        expect(it.next()).toEqual({ value: undefined, done: true });
    });
});
