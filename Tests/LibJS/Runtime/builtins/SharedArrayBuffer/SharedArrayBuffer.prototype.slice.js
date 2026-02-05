describe("errors", () => {
    test("called on non-SharedArrayBuffer object", () => {
        expect(() => {
            SharedArrayBuffer.prototype.slice(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Not an object of type SharedArrayBuffer");

        expect(() => {
            SharedArrayBuffer.prototype.slice.call(new ArrayBuffer());
        }).toThrowWithMessage(TypeError, "The array buffer object must be a SharedArrayBuffer");
    });
});

test("single parameter", () => {
    const buffer = new SharedArrayBuffer(16);
    const fullView = new Int32Array(buffer);

    // modify some value that we can check in the sliced buffer
    fullView[3] = 7;

    // slice the buffer and use a new int32 view to perform basic checks
    const slicedBuffer = buffer.slice(12);
    const slicedView = new Int32Array(slicedBuffer);

    expect(slicedView).toHaveLength(1);
    expect(slicedView[0]).toBe(7);
});

test("both parameters", () => {
    const buffer = new SharedArrayBuffer(16);
    const fullView = new Int32Array(buffer);

    // modify some value that we can check in the sliced buffer
    fullView[1] = 12;

    // slice the buffer and use a new int32 view to perform basic checks
    const slicedBuffer = buffer.slice(4, 8);
    const slicedView = new Int32Array(slicedBuffer);

    expect(slicedView).toHaveLength(1);
    expect(slicedView[0]).toBe(12);
});
