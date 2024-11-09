describe("errors", () => {
    test("invalid string", () => {
        expect(() => {
            Uint8Array.fromHex(3.14);
        }).toThrowWithMessage(TypeError, "3.14 is not a string");
    });

    test("odd number of characters", () => {
        expect(() => {
            Uint8Array.fromHex("a");
        }).toThrowWithMessage(SyntaxError, "Hex string must have an even length");
    });

    test("invalid alphabet", () => {
        expect(() => {
            Uint8Array.fromHex("qq");
        }).toThrowWithMessage(SyntaxError, "Hex string must only contain hex characters");
    });
});

describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Uint8Array.fromHex).toHaveLength(1);
    });

    const decodeEqual = (input, expected, options) => {
        const decoded = Uint8Array.fromHex(input, options);
        expect(decoded).toEqual(toUTF8Bytes(expected));
    };

    test("basic functionality", () => {
        decodeEqual("", "");
        decodeEqual("61", "a");
        decodeEqual("616263646566303132333435", "abcdef012345");
        decodeEqual("f09fa493", "🤓");
        decodeEqual("f09fa493666f6ff09f9696", "🤓foo🖖");
    });
});
