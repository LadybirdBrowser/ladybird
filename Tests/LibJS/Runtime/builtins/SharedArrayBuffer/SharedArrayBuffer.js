test("basic functionality", () => {
    expect(SharedArrayBuffer).toHaveLength(1);
    expect(SharedArrayBuffer.name).toBe("SharedArrayBuffer");
    expect(SharedArrayBuffer.prototype.constructor).toBe(SharedArrayBuffer);
    expect(new SharedArrayBuffer()).toBeInstanceOf(SharedArrayBuffer);
    expect(typeof new SharedArrayBuffer()).toBe("object");
});

test("SharedArrayBuffer constructor must be invoked with 'new'", () => {
    expect(() => {
        SharedArrayBuffer();
    }).toThrowWithMessage(TypeError, "SharedArrayBuffer constructor must be called with 'new'");
});

test("SharedArrayBuffer size limit", () => {
    expect(() => {
        new SharedArrayBuffer(2 ** 53);
    }).toThrowWithMessage(RangeError, "Invalid shared array buffer length");
});

test("SharedArrayBuffer cannot be detached", () => {
    const buffer = new SharedArrayBuffer(1);
    expect(() => {
        detachArrayBuffer(buffer);
    }).toThrowWithMessage(TypeError, "The array buffer object cannot be a SharedArrayBuffer");
    expect(buffer.byteLength).toBe(1);
});
