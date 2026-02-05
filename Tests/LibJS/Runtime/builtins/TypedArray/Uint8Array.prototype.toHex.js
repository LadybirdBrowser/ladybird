describe("errors", () => {
    test("called on non-Uint8Array object", () => {
        expect(() => {
            Uint8Array.prototype.toHex.call(1);
        }).toThrowWithMessage(TypeError, "Not an object of type Uint8Array");

        expect(() => {
            Uint8Array.prototype.toHex.call(new Uint16Array());
        }).toThrowWithMessage(TypeError, "Not an object of type Uint8Array");
    });

    test("detached ArrayBuffer", () => {
        let arrayBuffer = new ArrayBuffer(5, { maxByteLength: 10 });
        let typedArray = new Uint8Array(arrayBuffer, Uint8Array.BYTES_PER_ELEMENT, 1);
        detachArrayBuffer(arrayBuffer);

        expect(() => {
            typedArray.toHex();
        }).toThrowWithMessage(
            TypeError,
            "TypedArray contains a property which references a value at an index not contained within its buffer's bounds"
        );
    });

    test("ArrayBuffer out of bounds", () => {
        let arrayBuffer = new ArrayBuffer(Uint8Array.BYTES_PER_ELEMENT * 2, {
            maxByteLength: Uint8Array.BYTES_PER_ELEMENT * 4,
        });

        let typedArray = new Uint8Array(arrayBuffer, Uint8Array.BYTES_PER_ELEMENT, 1);
        arrayBuffer.resize(Uint8Array.BYTES_PER_ELEMENT);

        expect(() => {
            typedArray.toHex();
        }).toThrowWithMessage(
            TypeError,
            "TypedArray contains a property which references a value at an index not contained within its buffer's bounds"
        );
    });
});

describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Uint8Array.prototype.toHex).toHaveLength(0);
    });

    const encodeEqual = (input, expected) => {
        const encoded = toUTF8Bytes(input).toHex();
        expect(encoded).toBe(expected);
    };

    test("basic functionality", () => {
        encodeEqual("", "");
        encodeEqual("a", "61");
        encodeEqual("abcdef012345", "616263646566303132333435");
        encodeEqual("🤓", "f09fa493");
        encodeEqual("🤓foo🖖", "f09fa493666f6ff09f9696");
    });
});
