// Only a length-tracking view can be this long, since fixed-length views are capped at INT32_MAX bytes.
const length = 0x80000001;
const typedArray = new Uint8Array(new ArrayBuffer(length, { maxByteLength: length }));
typedArray[length - 2] = 11;
typedArray[length - 1] = 22;

test("copying into a fixed-length view", () => {
    [Uint8Array, Float64Array].forEach(T => {
        expect(() => new T(typedArray)).toThrowWithMessage(RangeError, "Invalid typed array length");
    });
});

test("lastIndexOf", () => {
    expect(typedArray.lastIndexOf(22)).toBe(length - 1);
    expect(typedArray.lastIndexOf(11, -2)).toBe(length - 2);
});

test("reduceRight", () => {
    expect(() => {
        typedArray.reduceRight((accumulator, value, index) => {
            expect(accumulator).toBe(0);
            expect(value).toBe(22);
            expect(index).toBe(length - 1);
            throw new Error("visited");
        }, 0);
    }).toThrowWithMessage(Error, "visited");

    expect(() => {
        typedArray.reduceRight((accumulator, value, index) => {
            expect(accumulator).toBe(22);
            expect(value).toBe(11);
            expect(index).toBe(length - 2);
            throw new Error("visited");
        });
    }).toThrowWithMessage(Error, "visited");
});

test("slice", () => {
    const result = typedArray.slice(-2);
    expect(result).toHaveLength(2);
    expect(result[0]).toBe(11);
    expect(result[1]).toBe(22);
});
