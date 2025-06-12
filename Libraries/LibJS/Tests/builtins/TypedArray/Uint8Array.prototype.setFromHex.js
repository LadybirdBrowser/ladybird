describe("errors", () => {
    test("called on non-Uint8Array object", () => {
        expect(() => {
            Uint8Array.prototype.setFromHex.call("");
        }).toThrowWithMessage(TypeError, "Not an object of type Uint8Array");

        expect(() => {
            Uint8Array.prototype.setFromHex.call(new Uint16Array());
        }).toThrowWithMessage(TypeError, "Not an object of type Uint8Array");
    });

    test("detached ArrayBuffer", () => {
        let arrayBuffer = new ArrayBuffer(5, { maxByteLength: 10 });
        let typedArray = new Uint8Array(arrayBuffer, Uint8Array.BYTES_PER_ELEMENT, 1);
        detachArrayBuffer(arrayBuffer);

        expect(() => {
            typedArray.setFromHex("");
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
            typedArray.setFromHex("");
        }).toThrowWithMessage(
            TypeError,
            "TypedArray contains a property which references a value at an index not contained within its buffer's bounds"
        );
    });

    test("invalid string", () => {
        expect(() => {
            new Uint8Array(10).setFromHex(3.14);
        }).toThrowWithMessage(TypeError, "3.14 is not a string");
    });

    test("odd number of characters", () => {
        expect(() => {
            new Uint8Array(10).setFromHex("a");
        }).toThrowWithMessage(SyntaxError, "Hex string must have an even length");
    });

    test("invalid alphabet", () => {
        expect(() => {
            new Uint8Array(10).setFromHex("qq");
        }).toThrowWithMessage(SyntaxError, "Hex string must only contain hex characters");
    });
});

describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Uint8Array.prototype.setFromHex).toHaveLength(1);
    });

    const decodeEqual = (input, expected) => {
        expected = toUTF8Bytes(expected);

        let array = new Uint8Array(expected.length);
        let result = array.setFromHex(input);

        expect(result.read).toBe(input.length);
        expect(result.written).toBe(expected.length);

        expect(array).toEqual(expected);
    };

    test("basic functionality", () => {
        decodeEqual("", "");
        decodeEqual("61", "a");
        decodeEqual("616263646566303132333435", "abcdef012345");
        decodeEqual("f09fa493", "🤓");
        decodeEqual("f09fa493666f6ff09f9696", "🤓foo🖖");
    });
});
