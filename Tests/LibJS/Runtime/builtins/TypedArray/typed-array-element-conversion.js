const NUMBER_TYPED_ARRAYS = [
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

const NUMBERS = [
    0,
    -0,
    1,
    -1,
    0.5,
    1.5,
    2.5,
    -2.5,
    127,
    128,
    255,
    255.5,
    256,
    -129,
    32767,
    32768,
    65535,
    65536,
    2 ** 31,
    -(2 ** 31),
    2 ** 32 - 1,
    2 ** 32,
    1 + 2 ** -11,
    1 + 3 * 2 ** -11,
    65520,
    1e300,
    -1e300,
    Infinity,
    -Infinity,
    NaN,
    1e-8,
];

// Elements that convert through a SharedArrayBuffer take the generic element-by-element path.
function sharedCopyOf(source) {
    const copy = new source.constructor(new SharedArrayBuffer(source.byteLength));
    copy.set(source);
    return copy;
}

function expectSameElements(actual, expected) {
    expect(actual.length).toBe(expected.length);
    for (let i = 0; i < expected.length; ++i) expect(Object.is(actual[i], expected[i])).toBeTrue();
}

describe("normal behavior", () => {
    test("converting between Number element types", () => {
        for (const SourceType of NUMBER_TYPED_ARRAYS) {
            const source = new SourceType(NUMBERS);
            for (const TargetType of NUMBER_TYPED_ARRAYS) {
                const expected = new TargetType(new SharedArrayBuffer(source.length * TargetType.BYTES_PER_ELEMENT));
                expected.set(sharedCopyOf(source));

                expectSameElements(new TargetType(source), expected);

                const target = new TargetType(source.length + 2);
                target.set(source, 1);
                expect(target[0]).toBe(0);
                expectSameElements(target.subarray(1, source.length + 1), expected);
                expect(target[source.length + 1]).toBe(0);
            }
        }
    });

    test("storing int32 values", () => {
        const values = [
            0,
            1,
            -1,
            127,
            128,
            -128,
            -129,
            255,
            256,
            300,
            32767,
            32768,
            -32769,
            65535,
            65536,
            2 ** 31 - 1,
            -(2 ** 31),
        ];
        expectSameElements(new Int8Array(values), [0, 1, -1, 127, -128, -128, 127, -1, 0, 44, -1, 0, -1, -1, 0, -1, 0]);
        expectSameElements(
            new Uint8Array(values),
            [0, 1, 255, 127, 128, 128, 127, 255, 0, 44, 255, 0, 255, 255, 0, 255, 0]
        );
        expectSameElements(
            new Uint8ClampedArray(values),
            [0, 1, 0, 127, 128, 0, 0, 255, 255, 255, 255, 255, 0, 255, 255, 255, 0]
        );
        expectSameElements(
            new Int16Array(values),
            [0, 1, -1, 127, 128, -128, -129, 255, 256, 300, 32767, -32768, 32767, -1, 0, -1, 0]
        );
        expectSameElements(
            new Uint16Array(values),
            [0, 1, 65535, 127, 128, 65408, 65407, 255, 256, 300, 32767, 32768, 32767, 65535, 0, 65535, 0]
        );
        expectSameElements(new Uint32Array([-1, -(2 ** 31), 2 ** 31 - 1]), [2 ** 32 - 1, 2 ** 31, 2 ** 31 - 1]);
        expectSameElements(new Int32Array([-1, -(2 ** 31), 2 ** 31 - 1]), [-1, -(2 ** 31), 2 ** 31 - 1]);
    });

    test("Uint8Clamped rounds half to even", () => {
        const source = new Float64Array([-1, 0.5, 1.5, 2.5, 254.5, 255.5, 300, NaN]);
        expectSameElements(new Uint8ClampedArray(source), [0, 0, 2, 2, 254, 255, 255, 0]);
    });

    test("Float16 rounds to nearest, ties to even", () => {
        const source = new Float64Array([1 + 2 ** -10, 1 + 2 ** -11, 1 + 3 * 2 ** -11, 65519, 65520, 2 ** -25]);
        expectSameElements(new Float16Array(source), [1 + 2 ** -10, 1, 1 + 2 ** -9, 65504, Infinity, 0]);
    });

    test("NaN elements become the canonical NaN", () => {
        const source = new Float64Array(2);
        const sourceBits = new Uint32Array(source.buffer);
        sourceBits.set([0x1234, 0x7ff01234, 0x5678, 0xfff85678]);

        const float32 = new Float32Array(source);
        expectSameElements(new Uint32Array(float32.buffer), [0x7fc00000, 0x7fc00000]);

        const float16 = new Float16Array(2);
        float16.set(source);
        expectSameElements(new Uint16Array(float16.buffer), [0x7e00, 0x7e00]);

        const float64 = new Float64Array(new Float32Array(new Uint32Array([0xff812345]).buffer));
        expectSameElements(new Uint32Array(float64.buffer), [0, 0x7ff80000]);
    });

    test("converting between BigInt element types", () => {
        const values = [0n, 1n, -1n, 2n ** 63n - 1n, -(2n ** 63n), 12345678901234567n];
        const unsigned = new BigUint64Array(new BigInt64Array(values));
        expectSameElements(unsigned, [0n, 1n, 2n ** 64n - 1n, 2n ** 63n - 1n, 2n ** 63n, 12345678901234567n]);

        const signed = new BigInt64Array(values.length + 1);
        signed.set(unsigned, 1);
        expectSameElements(signed, [0n, ...values]);
    });

    test("converting between views of the same buffer", () => {
        const buffer = new ArrayBuffer(16);
        const bytes = new Uint8Array(buffer);
        bytes.set([1, 2, 3, 4, 5, 6, 7, 8]);
        new Uint16Array(buffer).set(bytes.subarray(0, 4));
        expectSameElements(bytes.subarray(0, 8), [1, 0, 2, 0, 3, 0, 4, 0]);
    });
});
