describe("errors", () => {
    test("called on non-DataView object", () => {
        expect(() => {
            DataView.prototype.byteLength;
        }).toThrowWithMessage(TypeError, "Not an object of type DataView");
    });

    test("detached buffer", () => {
        let buffer = new ArrayBuffer(5, { maxByteLength: 10 });
        let view = new DataView(buffer);
        detachArrayBuffer(buffer);

        expect(() => {
            view.byteLength;
        }).toThrowWithMessage(
            TypeError,
            "DataView contains a property which references a value at an index not contained within its buffer's bounds"
        );
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        let buffer = new ArrayBuffer(124);
        expect(new DataView(buffer).byteLength).toBe(124);
        expect(new DataView(buffer, 0, 1).byteLength).toBe(1);
        expect(new DataView(buffer, 0, 64).byteLength).toBe(64);
        expect(new DataView(buffer, 0, 123).byteLength).toBe(123);

        buffer = new ArrayBuffer(5, { maxByteLength: 10 });
        expect(new DataView(buffer).byteLength).toBe(5);
        expect(new DataView(buffer, 0).byteLength).toBe(5);
        expect(new DataView(buffer, 3).byteLength).toBe(2);
        expect(new DataView(buffer, 5).byteLength).toBe(0);
    });
});
