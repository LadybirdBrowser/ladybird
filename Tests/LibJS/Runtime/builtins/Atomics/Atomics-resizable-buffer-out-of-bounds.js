// Atomics operation validate the index against the typed-array length before coercing their arguments. If an argument's
// valueOf shrinks the backing resizable ArrayBuffer so the target element now starts in bounds but ends out of bounds,
// the operation must throw a RangeError — rather than read or write past the (now shorter) buffer. See #10759.

// A length-tracking Int32Array over a 100-byte buffer: element 24 spans bytes 96..99. Shrinking the buffer to 97 leaves
// element 24 starting in bounds (96 < 97) but ending out of bounds (needs 99).
function shrinkingInt32Array(newByteLength) {
    const buffer = new ArrayBuffer(100, { maxByteLength: 100 });
    const array = new Int32Array(buffer);
    return {
        array,
        coercer: {
            valueOf() {
                buffer.resize(newByteLength);
                return 1;
            },
        },
    };
}

test("read-modify-write throws when the element straddles a buffer shrunk during coercion", () => {
    ["add", "and", "or", "sub", "xor", "exchange", "store"].forEach(op => {
        const { array, coercer } = shrinkingInt32Array(97);
        expect(() => {
            Atomics[op](array, 24, coercer);
        }).toThrow(RangeError);
    });
});

test("compareExchange throws when the element straddles a buffer shrunk during coercion", () => {
    const { array, coercer } = shrinkingInt32Array(97);
    expect(() => {
        Atomics.compareExchange(array, 24, coercer, 1);
    }).toThrow(RangeError);
});

test("load throws when the element straddles a buffer shrunk during index coercion", () => {
    const buffer = new ArrayBuffer(100, { maxByteLength: 100 });
    const array = new Int32Array(buffer);
    expect(() => {
        Atomics.load(array, {
            valueOf() {
                buffer.resize(97);
                return 24;
            },
        });
    }).toThrow(RangeError);
});

test("a larger element straddling a shrunk buffer also throws", () => {
    // BigInt64Array element 11 spans bytes 88..95; shrinking to 89 leaves it straddling.
    const buffer = new ArrayBuffer(96, { maxByteLength: 96 });
    const array = new BigInt64Array(buffer);
    expect(() => {
        Atomics.and(array, 11, {
            valueOf() {
                buffer.resize(89);
                return 1n;
            },
        });
    }).toThrow(RangeError);
});

test("a shrink that leaves the element fully out of bounds still throws RangeError", () => {
    const { array, coercer } = shrinkingInt32Array(50);
    expect(() => {
        Atomics.and(array, 24, coercer);
    }).toThrow(RangeError);
});
