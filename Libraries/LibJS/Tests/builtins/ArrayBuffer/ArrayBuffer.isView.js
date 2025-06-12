// Update when more typed arrays get added
const TYPED_ARRAYS = [
    Uint8Array,
    Uint16Array,
    Uint32Array,
    Int8Array,
    Int16Array,
    Int32Array,
    Float32Array,
    Float64Array,
];

test("basic functionality", () => {
    expect(ArrayBuffer.isView).toHaveLength(1);

    expect(ArrayBuffer.isView()).toBeFalse();
    expect(ArrayBuffer.isView(null)).toBeFalse();
    expect(ArrayBuffer.isView(undefined)).toBeFalse();
    expect(ArrayBuffer.isView([])).toBeFalse();
    expect(ArrayBuffer.isView({})).toBeFalse();
    expect(ArrayBuffer.isView(123)).toBeFalse();
    expect(ArrayBuffer.isView("foo")).toBeFalse();
    expect(ArrayBuffer.isView(new ArrayBuffer())).toBeFalse();
    TYPED_ARRAYS.forEach(T => {
        expect(ArrayBuffer.isView(new T())).toBeTrue();
    });
});
