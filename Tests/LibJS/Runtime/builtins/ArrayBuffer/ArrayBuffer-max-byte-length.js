// Buffers are capped at 2^32 bytes.
const maxLength = 2 ** 32;

test("buffers of the largest byte length", () => {
    expect(new ArrayBuffer(maxLength).byteLength).toBe(maxLength);
    expect(() => new ArrayBuffer(maxLength + 1)).toThrowWithMessage(RangeError, "Invalid array buffer length");

    const resizable = new ArrayBuffer(0, { maxByteLength: maxLength });
    resizable.resize(maxLength);
    expect(resizable.byteLength).toBe(maxLength);
    expect(() => new ArrayBuffer(0, { maxByteLength: maxLength + 1 })).toThrowWithMessage(
        RangeError,
        "Invalid array buffer length"
    );

    expect(new ArrayBuffer(8).transfer(maxLength).byteLength).toBe(maxLength);
    expect(() => new ArrayBuffer(8).transfer(maxLength + 1)).toThrowWithMessage(
        RangeError,
        "Invalid array buffer length"
    );

    expect(new SharedArrayBuffer(0, { maxByteLength: maxLength }).maxByteLength).toBe(maxLength);
    expect(() => new SharedArrayBuffer(0, { maxByteLength: maxLength + 1 })).toThrowWithMessage(
        RangeError,
        "Invalid shared array buffer length"
    );
});

test("views of a buffer of the largest byte length", () => {
    const buffer = new ArrayBuffer(maxLength);

    const view = new Uint8Array(buffer);
    view[maxLength - 1] = 42;
    expect(view).toHaveLength(maxLength);
    expect(view.byteLength).toBe(maxLength);
    expect(view[maxLength - 1]).toBe(42);
    expect(view.at(-1)).toBe(42);
    expect(view[-1]).toBeUndefined();
    view[-1] = 1;
    expect(view[maxLength - 1]).toBe(42);
    expect(view.lastIndexOf(42)).toBe(maxLength - 1);
    expect(view.subarray(-1).byteOffset).toBe(maxLength - 1);
    expect(Array.from(view.slice(-1))).toEqual([42]);

    const empty = new Uint8Array(buffer, maxLength);
    expect(empty).toHaveLength(0);
    expect(empty.byteOffset).toBe(maxLength);
    expect(() => new Uint8Array(buffer, 0, maxLength + 1)).toThrow(RangeError);

    expect(new Float64Array(buffer)).toHaveLength(maxLength / 8);

    const dataView = new DataView(buffer);
    expect(dataView.byteLength).toBe(maxLength);
    expect(dataView.getUint8(maxLength - 1)).toBe(42);
    expect(new DataView(buffer, maxLength).byteOffset).toBe(maxLength);
    expect(() => new DataView(buffer, 0, maxLength + 1)).toThrow(RangeError);
});

test("length-tracking views of a resizable buffer of the largest byte length", () => {
    const buffer = new ArrayBuffer(maxLength, { maxByteLength: maxLength });

    const view = new Uint8Array(buffer);
    view[maxLength - 1] = 7;
    expect(view).toHaveLength(maxLength);
    expect(view[maxLength - 1]).toBe(7);

    expect(new DataView(buffer).byteLength).toBe(maxLength);
});
