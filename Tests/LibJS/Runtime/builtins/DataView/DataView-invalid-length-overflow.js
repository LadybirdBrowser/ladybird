test("Issue #13451, integer overflow in offset + view_byte_length", () => {
    const arrayBuffer = new ArrayBuffer(1);
    expect(() => {
        new DataView(arrayBuffer, 1, 1024 * 1024 * 1024 * 4 - 1);
    }).toThrowWithMessage(RangeError, "Invalid DataView length");
});
