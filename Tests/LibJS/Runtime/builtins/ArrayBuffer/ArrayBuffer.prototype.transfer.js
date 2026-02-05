describe("errors", () => {
    test("called on non-ArrayBuffer object", () => {
        expect(() => {
            ArrayBuffer.prototype.transfer(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Not an object of type ArrayBuffer");
    });

    test("called on SharedArrayBuffer object", () => {
        expect(() => {
            ArrayBuffer.prototype.transfer.call(new SharedArrayBuffer());
        }).toThrowWithMessage(TypeError, "The array buffer object cannot be a SharedArrayBuffer");
    });

    test("detached buffer", () => {
        let buffer = new ArrayBuffer(5);
        detachArrayBuffer(buffer);

        expect(() => {
            buffer.transfer();
        }).toThrowWithMessage(TypeError, "ArrayBuffer is detached");
    });
});

const readBuffer = buffer => {
    let array = new Uint8Array(buffer, 0, buffer.byteLength / Uint8Array.BYTES_PER_ELEMENT);
    let values = [];

    for (let value of array) {
        values.push(Number(value));
    }

    return values;
};

const writeBuffer = (buffer, values) => {
    let array = new Uint8Array(buffer, 0, buffer.byteLength / Uint8Array.BYTES_PER_ELEMENT);
    array.set(values);
};

describe("normal behavior", () => {
    test("old buffer is detached", () => {
        let buffer = new ArrayBuffer(5);
        let newBuffer = buffer.transfer();

        expect(buffer.detached).toBeTrue();
        expect(newBuffer.detached).toBeFalse();
    });

    test("resizability is preserved", () => {
        let buffer = new ArrayBuffer(5, { maxByteLength: 10 });
        let newBuffer = buffer.transfer();

        expect(buffer.resizable).toBeTrue();
        expect(newBuffer.resizable).toBeTrue();
    });

    test("data is transferred", () => {
        let buffer = new ArrayBuffer(5);
        writeBuffer(buffer, [1, 2, 3, 4, 5]);

        let newBuffer = buffer.transfer();
        const values = readBuffer(newBuffer);

        expect(values).toEqual([1, 2, 3, 4, 5]);
    });

    test("length may be limited", () => {
        let buffer = new ArrayBuffer(5);
        writeBuffer(buffer, [1, 2, 3, 4, 5]);

        let newBuffer = buffer.transfer(3);
        const values = readBuffer(newBuffer);

        expect(values).toEqual([1, 2, 3]);
    });
});
