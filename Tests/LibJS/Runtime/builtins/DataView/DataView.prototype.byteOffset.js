describe("errors", () => {
    test("called on non-DataView object", () => {
        expect(() => {
            DataView.prototype.byteOffset;
        }).toThrowWithMessage(TypeError, "Not an object of type DataView");
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        const buffer = new ArrayBuffer(124);
        expect(new DataView(buffer).byteOffset).toBe(0);
        expect(new DataView(buffer, 1).byteOffset).toBe(1);
        expect(new DataView(buffer, 64).byteOffset).toBe(64);
        expect(new DataView(buffer, 123).byteOffset).toBe(123);
    });

    test("detached buffer", () => {
        let buffer = new ArrayBuffer(5, { maxByteLength: 10 });
        let view = new DataView(buffer);
        detachArrayBuffer(buffer);
        expect(view.byteOffset).toBe(0);
    });
});
