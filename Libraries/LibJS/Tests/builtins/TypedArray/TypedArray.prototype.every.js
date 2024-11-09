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
    function errorTests(T) {
        test(`requires at least one argument (${T.name})`, () => {
            expect(() => {
                new T().every();
            }).toThrowWithMessage(
                TypeError,
                "TypedArray.prototype.every() requires at least one argument"
            );
        });

        test(`callback must be a function (${T.name})`, () => {
            expect(() => {
                new T().every(undefined);
            }).toThrowWithMessage(TypeError, "undefined is not a function");
        });

        test(`ArrayBuffer out of bounds  (${T.name})`, () => {
            let arrayBuffer = new ArrayBuffer(T.BYTES_PER_ELEMENT * 2, {
                maxByteLength: T.BYTES_PER_ELEMENT * 4,
            });

            let typedArray = new T(arrayBuffer, T.BYTES_PER_ELEMENT, 1);
            arrayBuffer.resize(T.BYTES_PER_ELEMENT);

            expect(() => {
                typedArray.every(value => value === 0);
            }).toThrowWithMessage(
                TypeError,
                "TypedArray contains a property which references a value at an index not contained within its buffer's bounds"
            );
        });
    }

    TYPED_ARRAYS.forEach(T => errorTests(T));
    BIGINT_TYPED_ARRAYS.forEach(T => errorTests(T));
});

test("length is 1", () => {
    TYPED_ARRAYS.forEach(T => {
        expect(T.prototype.every).toHaveLength(1);
    });

    BIGINT_TYPED_ARRAYS.forEach(T => {
        expect(T.prototype.every).toHaveLength(1);
    });
});

test("basic functionality", () => {
    TYPED_ARRAYS.forEach(T => {
        const typedArray = new T([2, 4, 6]);
        expect(typedArray.every(value => value === 2)).toBeFalse();
        expect(typedArray.every(value => value === 4)).toBeFalse();
        expect(typedArray.every(value => value === 6)).toBeFalse();
        expect(typedArray.every(value => value % 2 === 0)).toBeTrue();
        expect(typedArray.every(value => value % 2 === 1)).toBeFalse();
        expect(typedArray.every(value => value < 2)).toBeFalse();
        expect(typedArray.every(value => value > 2)).toBeFalse();
        expect(typedArray.every(value => value >= 2)).toBeTrue();
    });

    BIGINT_TYPED_ARRAYS.forEach(T => {
        const typedArray = new T([2n, 4n, 6n]);
        expect(typedArray.every(value => value === 2n)).toBeFalse();
        expect(typedArray.every(value => value === 4n)).toBeFalse();
        expect(typedArray.every(value => value === 6n)).toBeFalse();
        expect(typedArray.every(value => value % 2n === 0n)).toBeTrue();
        expect(typedArray.every(value => value % 2n === 1n)).toBeFalse();
        expect(typedArray.every(value => value < 2n)).toBeFalse();
        expect(typedArray.every(value => value > 2n)).toBeFalse();
        expect(typedArray.every(value => value >= 2n)).toBeTrue();
    });
});
