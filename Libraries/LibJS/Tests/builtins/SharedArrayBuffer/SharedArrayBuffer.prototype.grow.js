describe("errors", () => {
    test("called on non-SharedArrayBuffer object", () => {
        expect(() => {
            SharedArrayBuffer.prototype.grow(10);
        }).toThrowWithMessage(TypeError, "Not an object of type SharedArrayBuffer");
    });

    test("fixed buffer", () => {
        let buffer = new SharedArrayBuffer(5);

        expect(() => {
            buffer.grow(10);
        }).toThrowWithMessage(TypeError, "ArrayBuffer is not resizable");
    });

    test("invalid new byte length", () => {
        let buffer = new SharedArrayBuffer(5, { maxByteLength: 10 });

        expect(() => {
            buffer.grow(-1);
        }).toThrowWithMessage(RangeError, "Index must be a positive integer");
    });

    test("new byte length less than previous byte length", () => {
        let buffer = new SharedArrayBuffer(5, { maxByteLength: 10 });

        expect(() => {
            buffer.grow(4);
        }).toThrowWithMessage(
            RangeError,
            "SharedArrayBuffer byte length of 4 is less than the previous byte length of 5"
        );
    });

    test("new byte length exceeds maximum size", () => {
        let buffer = new SharedArrayBuffer(5, { maxByteLength: 10 });

        expect(() => {
            buffer.grow(11);
        }).toThrowWithMessage(RangeError, "ArrayBuffer byte length of 11 exceeds the max byte length of 10");
    });
});

describe("normal behavior", () => {
    test("resizable buffer", () => {
        let buffer = new SharedArrayBuffer(5, { maxByteLength: 10 });
        expect(buffer.byteLength).toBe(5);
        expect(buffer.maxByteLength).toBe(10);

        for (let i = buffer.byteLength; i <= buffer.maxByteLength; ++i) {
            buffer.grow(i);
            expect(buffer.byteLength).toBe(i);
        }
    });

    test("enlarged buffers filled with zeros", () => {
        let buffer = new SharedArrayBuffer(5, { maxByteLength: 10 });

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

        buffer.grow(8);
        expect(readBuffer()).toEqual([1, 2, 3, 4, 5, 0, 0, 0]);

        writeBuffer([1, 2, 3, 4, 5, 6, 7, 8]);
        expect(readBuffer()).toEqual([1, 2, 3, 4, 5, 6, 7, 8]);

        buffer.grow(10);
        expect(readBuffer()).toEqual([1, 2, 3, 4, 5, 6, 7, 8, 0, 0]);
    });
});
