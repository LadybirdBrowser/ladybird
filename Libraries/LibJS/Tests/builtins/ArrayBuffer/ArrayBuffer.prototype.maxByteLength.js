describe("errors", () => {
    test("called on non-ArrayBuffer object", () => {
        expect(() => {
            ArrayBuffer.prototype.maxByteLength;
        }).toThrowWithMessage(TypeError, "Not an object of type ArrayBuffer");
    });
});

describe("normal behavior", () => {
    test("detached buffer", () => {
        let buffer = new ArrayBuffer(5);
        detachArrayBuffer(buffer);
        expect(buffer.maxByteLength).toBe(0);

        buffer = new ArrayBuffer(5, { maxByteLength: 10 });
        detachArrayBuffer(buffer);
        expect(buffer.maxByteLength).toBe(0);
    });

    test("fixed buffer", () => {
        let buffer = new ArrayBuffer(5);
        expect(buffer.maxByteLength).toBe(5);
    });

    test("resizable buffer", () => {
        let buffer = new ArrayBuffer(5, { maxByteLength: 10 });
        expect(buffer.maxByteLength).toBe(10);
    });
});
