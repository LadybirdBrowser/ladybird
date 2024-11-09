describe("errors", () => {
    test("called on non-SharedArrayBuffer object", () => {
        expect(() => {
            SharedArrayBuffer.prototype.byteLength;
        }).toThrowWithMessage(TypeError, "Not an object of type SharedArrayBuffer");

        let byteLength = Object.getOwnPropertyDescriptor(SharedArrayBuffer.prototype, "byteLength");
        let getter = byteLength.get;

        expect(() => {
            getter.call(new ArrayBuffer());
        }).toThrowWithMessage(TypeError, "The array buffer object must be a SharedArrayBuffer");
    });
});

test("basic functionality", () => {
    expect(new SharedArrayBuffer().byteLength).toBe(0);
    expect(new SharedArrayBuffer(1).byteLength).toBe(1);
    expect(new SharedArrayBuffer(64).byteLength).toBe(64);
    expect(new SharedArrayBuffer(123).byteLength).toBe(123);
});
