describe("errors", () => {
    test("called on non-ArrayBuffer object", () => {
        expect(() => {
            ArrayBuffer.prototype.resize(10);
        }).toThrowWithMessage(TypeError, "Not an object of type ArrayBuffer");
    });

    test("fixed buffer", () => {
        let buffer = new ArrayBuffer(5);
        detachArrayBuffer(buffer);

        expect(() => {
            buffer.resize(10);
        }).toThrowWithMessage(TypeError, "ArrayBuffer is not resizable");
    });

    test("detached buffer", () => {
        let buffer = new ArrayBuffer(5, { maxByteLength: 10 });
        detachArrayBuffer(buffer);

        expect(() => {
            buffer.resize(10);
        }).toThrowWithMessage(TypeError, "ArrayBuffer is detached");
    });

    test("invalid new byte length", () => {
        let buffer = new ArrayBuffer(5, { maxByteLength: 10 });

        expect(() => {
            buffer.resize(-1);
        }).toThrowWithMessage(RangeError, "Index must be a positive integer");
    });

    test("new byte length exceeds maximum size", () => {
        let buffer = new ArrayBuffer(5, { maxByteLength: 10 });

        expect(() => {
            buffer.resize(11);
        }).toThrowWithMessage(
            RangeError,
            "ArrayBuffer byte length of 11 exceeds the max byte length of 10"
        );
    });
});

describe("normal behavior", () => {
    test("resizable buffer", () => {
        let buffer = new ArrayBuffer(5, { maxByteLength: 10 });
        expect(buffer.byteLength).toBe(5);
        expect(buffer.maxByteLength).toBe(10);

        for (let i = 0; i <= buffer.maxByteLength; ++i) {
            buffer.resize(i);
            expect(buffer.byteLength).toBe(i);
        }
    });

    test("enlarged buffers filled with zeros", () => {
        let buffer = new ArrayBuffer(5, { maxByteLength: 10 });

        const readBuffer = () => {
            let array = new Uint8Array(buffer, 0, buffer.byteLength / Uint8Array.BYTES_PER_ELEMENT);
            let values = [];

            for (let value of array) {
                values.push(Number(value));
            }

            return values;
        };

        const writeBuffer = values => {
            let array = new Uint8Array(buffer, 0, buffer.byteLength / Uint8Array.BYTES_PER_ELEMENT);
            array.set(values);
        };

        expect(readBuffer()).toEqual([0, 0, 0, 0, 0]);

        writeBuffer([1, 2, 3, 4, 5]);
        expect(readBuffer()).toEqual([1, 2, 3, 4, 5]);

        buffer.resize(8);
        expect(readBuffer()).toEqual([1, 2, 3, 4, 5, 0, 0, 0]);

        writeBuffer([1, 2, 3, 4, 5, 6, 7, 8]);
        expect(readBuffer()).toEqual([1, 2, 3, 4, 5, 6, 7, 8]);

        buffer.resize(10);
        expect(readBuffer()).toEqual([1, 2, 3, 4, 5, 6, 7, 8, 0, 0]);
    });
});
