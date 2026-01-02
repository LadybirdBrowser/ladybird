describe("errors", () => {
    test("called on non-SharedArrayBuffer object", () => {
        expect(() => {
            SharedArrayBuffer.prototype.maxByteLength;
        }).toThrowWithMessage(TypeError, "Not an object of type SharedArrayBuffer");
    });
});

describe("normal behavior", () => {
    test("fixed buffer", () => {
        let buffer = new SharedArrayBuffer(5);
        expect(buffer.maxByteLength).toBe(5);
    });

    test("resizable buffer", () => {
        let buffer = new SharedArrayBuffer(5, { maxByteLength: 10 });
        expect(buffer.maxByteLength).toBe(10);
    });
});
