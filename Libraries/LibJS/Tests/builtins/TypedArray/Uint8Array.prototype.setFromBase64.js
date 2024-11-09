describe("errors", () => {
    test("called on non-Uint8Array object", () => {
        expect(() => {
            Uint8Array.prototype.setFromBase64.call("");
        }).toThrowWithMessage(TypeError, "Not an object of type Uint8Array");

        expect(() => {
            Uint8Array.prototype.setFromBase64.call(new Uint16Array());
        }).toThrowWithMessage(TypeError, "Not an object of type Uint8Array");
    });

    test("detached ArrayBuffer", () => {
        let arrayBuffer = new ArrayBuffer(5, { maxByteLength: 10 });
        let typedArray = new Uint8Array(arrayBuffer, Uint8Array.BYTES_PER_ELEMENT, 1);
        detachArrayBuffer(arrayBuffer);

        expect(() => {
            typedArray.setFromBase64("");
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
            typedArray.setFromBase64("");
        }).toThrowWithMessage(
            TypeError,
            "TypedArray contains a property which references a value at an index not contained within its buffer's bounds"
        );
    });

    test("invalid string", () => {
        expect(() => {
            new Uint8Array(10).setFromBase64(3.14);
        }).toThrowWithMessage(TypeError, "3.14 is not a string");
    });

    test("invalid options object", () => {
        expect(() => {
            new Uint8Array(10).setFromBase64("", 3.14);
        }).toThrowWithMessage(TypeError, "Options is not an object");
    });

    test("invalid alphabet option", () => {
        expect(() => {
            new Uint8Array(10).setFromBase64("", { alphabet: 3.14 });
        }).toThrowWithMessage(TypeError, "3.14 is not a valid value for option alphabet");

        expect(() => {
            new Uint8Array(10).setFromBase64("", { alphabet: "foo" });
        }).toThrowWithMessage(TypeError, "foo is not a valid value for option alphabet");
    });

    test("invalid lastChunkHandling option", () => {
        expect(() => {
            new Uint8Array(10).setFromBase64("", { lastChunkHandling: 3.14 });
        }).toThrowWithMessage(TypeError, "3.14 is not a valid value for option lastChunkHandling");

        expect(() => {
            new Uint8Array(10).setFromBase64("", { lastChunkHandling: "foo" });
        }).toThrowWithMessage(TypeError, "foo is not a valid value for option lastChunkHandling");
    });

    test("strict mode with trailing data", () => {
        expect(() => {
            new Uint8Array(10).setFromBase64("Zm9va", { lastChunkHandling: "strict" });
        }).toThrowWithMessage(SyntaxError, "Invalid trailing data");
    });

    test("invalid padding", () => {
        expect(() => {
            new Uint8Array(10).setFromBase64("Zm9v=", { lastChunkHandling: "strict" });
        }).toThrowWithMessage(SyntaxError, "Unexpected padding character");

        expect(() => {
            new Uint8Array(10).setFromBase64("Zm9vaa=", { lastChunkHandling: "strict" });
        }).toThrowWithMessage(SyntaxError, "Incomplete number of padding characters");

        expect(() => {
            new Uint8Array(10).setFromBase64("Zm9vaa=a", { lastChunkHandling: "strict" });
        }).toThrowWithMessage(SyntaxError, "Unexpected padding character");
    });

    test("invalid alphabet", () => {
        expect(() => {
            new Uint8Array(10).setFromBase64("-", { lastChunkHandling: "strict" });
        }).toThrowWithMessage(SyntaxError, "Invalid character '-'");

        expect(() => {
            new Uint8Array(10).setFromBase64("+", {
                alphabet: "base64url",
                lastChunkHandling: "strict",
            });
        }).toThrowWithMessage(SyntaxError, "Invalid character '+'");
    });

    test("overlong chunk", () => {
        expect(() => {
            new Uint8Array(10).setFromBase64("Zh==", { lastChunkHandling: "strict" });
        }).toThrowWithMessage(SyntaxError, "Extra bits found at end of chunk");

        expect(() => {
            new Uint8Array(10).setFromBase64("Zm9=", { lastChunkHandling: "strict" });
        }).toThrowWithMessage(SyntaxError, "Extra bits found at end of chunk");
    });
});

describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Uint8Array.prototype.setFromBase64).toHaveLength(1);
    });

    const decodeEqual = (input, expected, options, expectedInputBytesRead) => {
        expected = toUTF8Bytes(expected);

        let array = new Uint8Array(expected.length);
        let result = array.setFromBase64(input, options);

        expect(result.read).toBe(expectedInputBytesRead || input.length);
        expect(result.written).toBe(expected.length);

        expect(array).toEqual(expected);
    };

    test("basic functionality", () => {
        decodeEqual("", "");
        decodeEqual("Zg==", "f");
        decodeEqual("Zm8=", "fo");
        decodeEqual("Zm9v", "foo");
        decodeEqual("Zm9vYg==", "foob");
        decodeEqual("Zm9vYmE=", "fooba");
        decodeEqual("Zm9vYmFy", "foobar");

        decodeEqual("8J+kkw==", "🤓");
        decodeEqual("8J+kk2Zvb/CflpY=", "🤓foo🖖");
    });

    test("base64url alphabet", () => {
        const options = { alphabet: "base64url" };

        decodeEqual("", "", options);
        decodeEqual("Zg==", "f", options);
        decodeEqual("Zm8=", "fo", options);
        decodeEqual("Zm9v", "foo", options);
        decodeEqual("Zm9vYg==", "foob", options);
        decodeEqual("Zm9vYmE=", "fooba", options);
        decodeEqual("Zm9vYmFy", "foobar", options);

        decodeEqual("8J-kkw==", "🤓", options);
        decodeEqual("8J-kk2Zvb_CflpY=", "🤓foo🖖", options);
    });

    test("stop-before-partial lastChunkHandling", () => {
        const options = { lastChunkHandling: "stop-before-partial" };

        decodeEqual("Zm9v", "foo", options, 4);
        decodeEqual("Zm9va", "foo", options, 4);
        decodeEqual("Zm9vaa", "foo", options, 4);
    });
});
