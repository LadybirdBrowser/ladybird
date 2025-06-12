test("Issue #9336, integer overflow in get_view_value", () => {
    const dataView = new DataView(new ArrayBuffer(16));
    expect(() => {
        dataView.getUint32(0xfffffffc);
    }).toThrowWithMessage(
        RangeError,
        "Data view byte offset 4294967292 is out of range for buffer with length 16"
    );
});

test("Issue #9338, integer overflow in set_view_value", () => {
    const dataView = new DataView(new ArrayBuffer(16));
    expect(() => {
        dataView.setUint32(0xfffffffc, 0);
    }).toThrowWithMessage(
        RangeError,
        "Data view byte offset 4294967292 is out of range for buffer with length 16"
    );
});

test("Issue #9338, integer overflow in set_view_value - zero-length DataView", () => {
    const dataView = new DataView(new ArrayBuffer(4), 4);
    expect(() => {
        dataView.setUint32(0xfffffffc, 0);
    }).toThrowWithMessage(
        RangeError,
        "Data view byte offset 4294967292 is out of range for buffer with length 0"
    );
});
