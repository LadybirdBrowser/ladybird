test("construct Uint32Array with zero-length ArrayBuffer and overflowing offset", () => {
    expect(() => new Uint32Array(new ArrayBuffer(0), 4, 1024 * 1024 * 1024 - 1)).toThrow(
        RangeError
    );
});
