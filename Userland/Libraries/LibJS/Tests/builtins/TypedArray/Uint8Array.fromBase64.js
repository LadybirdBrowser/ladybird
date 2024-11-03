describe("errors", () => {
    test("invalid string", () => {
        expect(() => {
            Uint8Array.fromBase64(3.14);
        }).toThrowWithMessage(TypeError, "3.14 is not a string");
    });

    test("invalid options object", () => {
        expect(() => {
            Uint8Array.fromBase64("", 3.14);
        }).toThrowWithMessage(TypeError, "Options is not an object");
    });

    test("invalid alphabet option", () => {
        expect(() => {
            Uint8Array.fromBase64("", { alphabet: 3.14 });
        }).toThrowWithMessage(TypeError, "3.14 is not a valid value for option alphabet");

        expect(() => {
            Uint8Array.fromBase64("", { alphabet: "foo" });
        }).toThrowWithMessage(TypeError, "foo is not a valid value for option alphabet");
    });

    test("invalid lastChunkHandling option", () => {
        expect(() => {
            Uint8Array.fromBase64("", { lastChunkHandling: 3.14 });
        }).toThrowWithMessage(TypeError, "3.14 is not a valid value for option lastChunkHandling");

        expect(() => {
            Uint8Array.fromBase64("", { lastChunkHandling: "foo" });
        }).toThrowWithMessage(TypeError, "foo is not a valid value for option lastChunkHandling");
    });

    test("strict mode with trailing data", () => {
        expect(() => {
            Uint8Array.fromBase64("Zm9va", { lastChunkHandling: "strict" });
        }).toThrowWithMessage(SyntaxError, "Invalid trailing data");
    });

    test("invalid padding", () => {
        expect(() => {
            Uint8Array.fromBase64("Zm9v=", { lastChunkHandling: "strict" });
        }).toThrowWithMessage(SyntaxError, "Unexpected padding character");

        expect(() => {
            Uint8Array.fromBase64("Zm9vaa=", { lastChunkHandling: "strict" });
        }).toThrowWithMessage(SyntaxError, "Incomplete number of padding characters");

        expect(() => {
            Uint8Array.fromBase64("Zm9vaa=a", { lastChunkHandling: "strict" });
        }).toThrowWithMessage(SyntaxError, "Unexpected padding character");
    });

    test("invalid alphabet", () => {
        expect(() => {
            Uint8Array.fromBase64("-", { lastChunkHandling: "strict" });
        }).toThrowWithMessage(SyntaxError, "Invalid character '-'");

        expect(() => {
            Uint8Array.fromBase64("+", { alphabet: "base64url", lastChunkHandling: "strict" });
        }).toThrowWithMessage(SyntaxError, "Invalid character '+'");
    });

    test("overlong chunk", () => {
        expect(() => {
            Uint8Array.fromBase64("Zh==", { lastChunkHandling: "strict" });
        }).toThrowWithMessage(SyntaxError, "Extra bits found at end of chunk");

        expect(() => {
            Uint8Array.fromBase64("Zm9=", { lastChunkHandling: "strict" });
        }).toThrowWithMessage(SyntaxError, "Extra bits found at end of chunk");
    });
});

describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Uint8Array.fromBase64).toHaveLength(1);
    });

    const decodeEqual = (input, expected, options) => {
        const decoded = Uint8Array.fromBase64(input, options);
        expect(decoded).toEqual(toUTF8Bytes(expected));
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

        decodeEqual("b2g_", "oh?", options);
    });

    test("strict mode with base64url alphabet", () => {
        const options = { alphabet: "base64url", lastChunkHandling: "strict" };

        decodeEqual("", "", options);
        decodeEqual("Zg==", "f", options);
        decodeEqual("Zm8=", "fo", options);
        decodeEqual("Zm9v", "foo", options);
        decodeEqual("Zm9vYg==", "foob", options);
        decodeEqual("Zm9vYmE=", "fooba", options);
        decodeEqual("Zm9vYmFy", "foobar", options);

        decodeEqual("8J-kkw==", "🤓", options);
        decodeEqual("8J-kk2Zvb_CflpY=", "🤓foo🖖", options);

        decodeEqual("b2g_", "oh?", options);
    });

    test("stop-before-partial lastChunkHandling", () => {
        const options = { lastChunkHandling: "stop-before-partial" };

        decodeEqual("Zm9v", "foo", options);
        decodeEqual("Zm9va", "foo", options);
        decodeEqual("Zm9vaa", "foo", options);
    });
});
