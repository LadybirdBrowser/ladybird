describe("errors", () => {
    test("called on non-ArrayBuffer object", () => {
        expect(() => {
            ArrayBuffer.prototype.resizable;
        }).toThrowWithMessage(TypeError, "Not an object of type ArrayBuffer");
    });
});

describe("normal behavior", () => {
    test("fixed buffer", () => {
        let buffer = new ArrayBuffer(5);
        expect(buffer.resizable).toBeFalse();
    });

    test("resizable buffer", () => {
        let buffer = new ArrayBuffer(5, { maxByteLength: 10 });
        expect(buffer.resizable).toBeTrue();
    });
});
