test("basic functionality", () => {
    expect(ArrayBuffer).toHaveLength(1);
    expect(ArrayBuffer.name).toBe("ArrayBuffer");
    expect(ArrayBuffer.prototype.constructor).toBe(ArrayBuffer);
    expect(new ArrayBuffer()).toBeInstanceOf(ArrayBuffer);
    expect(typeof new ArrayBuffer()).toBe("object");
});

test("ArrayBuffer constructor must be invoked with 'new'", () => {
    expect(() => {
        ArrayBuffer();
    }).toThrowWithMessage(TypeError, "ArrayBuffer constructor must be called with 'new'");
});

test("ArrayBuffer size limit", () => {
    expect(() => {
        new ArrayBuffer(2 ** 53);
    }).toThrowWithMessage(RangeError, "Invalid array buffer length");
});

test("invalid ArrayBuffer maximum size option", () => {
    expect(() => {
        new ArrayBuffer(10, { maxByteLength: -1 });
    }).toThrowWithMessage(RangeError, "Index must be a positive integer");
});

test("ArrayBuffer size exceeds maximum size", () => {
    expect(() => {
        new ArrayBuffer(10, { maxByteLength: 5 });
    }).toThrowWithMessage(
        RangeError,
        "ArrayBuffer byte length of 10 exceeds the max byte length of 5"
    );
});
