test("some oversized typed arrays", () => {
    expect(() => new Uint8Array(2 * 1024 * 1024 * 1024)).toThrowWithMessage(
        RangeError,
        "Invalid typed array length"
    );
    expect(() => new Uint16Array(2 * 1024 * 1024 * 1024)).toThrowWithMessage(
        RangeError,
        "Invalid typed array length"
    );
    expect(() => new Uint32Array(1024 * 1024 * 1024)).toThrowWithMessage(
        RangeError,
        "Invalid typed array length"
    );
    expect(() => new Uint32Array(4 * 1024 * 1024 * 1024)).toThrowWithMessage(
        RangeError,
        "Invalid typed array length"
    );
});
