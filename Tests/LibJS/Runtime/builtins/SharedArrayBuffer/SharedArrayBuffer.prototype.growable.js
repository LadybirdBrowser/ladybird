describe("errors", () => {
    test("called on non-SharedArrayBuffer object", () => {
        expect(() => {
            SharedArrayBuffer.prototype.growable;
        }).toThrowWithMessage(TypeError, "Not an object of type SharedArrayBuffer");
    });
});

describe("normal behavior", () => {
    test("fixed buffer", () => {
        let buffer = new SharedArrayBuffer(5);
        expect(buffer.growable).toBeFalse();
    });

    test("growable buffer", () => {
        let buffer = new SharedArrayBuffer(5, { maxByteLength: 10 });
        expect(buffer.growable).toBeTrue();
    });
});
