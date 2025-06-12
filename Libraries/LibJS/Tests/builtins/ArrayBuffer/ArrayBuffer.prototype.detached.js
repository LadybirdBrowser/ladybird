describe("errors", () => {
    test("called on non-ArrayBuffer object", () => {
        expect(() => {
            ArrayBuffer.prototype.detached;
        }).toThrowWithMessage(TypeError, "Not an object of type ArrayBuffer");
    });

    test("called on SharedArrayBuffer object", () => {
        let detached = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, "detached");
        let getter = detached.get;

        expect(() => {
            getter.call(new SharedArrayBuffer());
        }).toThrowWithMessage(TypeError, "The array buffer object cannot be a SharedArrayBuffer");
    });
});

describe("normal behavior", () => {
    test("not detached", () => {
        let buffer = new ArrayBuffer();
        expect(buffer.detached).toBeFalse();
    });

    test("detached", () => {
        let buffer = new ArrayBuffer();
        detachArrayBuffer(buffer);

        expect(buffer.detached).toBeTrue();
    });
});
