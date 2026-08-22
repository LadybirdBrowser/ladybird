describe("errors", () => {
    test("called on non-Uint8Array object", () => {
        expect(() => {
            Uint8Array.prototype.toBase64.call(1);
        }).toThrowWithMessage(TypeError, "Not an object of type Uint8Array");

        expect(() => {
            Uint8Array.prototype.toBase64.call(new Uint16Array());
        }).toThrowWithMessage(TypeError, "Not an object of type Uint8Array");
    });

    test("invalid options object", () => {
        expect(() => {
            new Uint8Array().toBase64(3.14);
        }).toThrowWithMessage(TypeError, "Options is not an object");
    });

    test("invalid alphabet option", () => {
        expect(() => {
            new Uint8Array().toBase64({ alphabet: 3.14 });
        }).toThrowWithMessage(TypeError, "3.14 is not a valid value for option alphabet");

        expect(() => {
            new Uint8Array().toBase64({ alphabet: "foo" });
        }).toThrowWithMessage(TypeError, "foo is not a valid value for option alphabet");
    });

    test("detached ArrayBuffer", () => {
        let arrayBuffer = new ArrayBuffer(5, { maxByteLength: 10 });
        let typedArray = new Uint8Array(arrayBuffer, Uint8Array.BYTES_PER_ELEMENT, 1);
        detachArrayBuffer(arrayBuffer);

        expect(() => {
            typedArray.toBase64();
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
            typedArray.toBase64();
        }).toThrowWithMessage(
            TypeError,
            "TypedArray contains a property which references a value at an index not contained within its buffer's bounds"
        );
    });
});

describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Uint8Array.prototype.toBase64).toHaveLength(0);
    });

    const encodeEqual = (input, expected, options) => {
        const encoded = toUTF8Bytes(input).toBase64(options);
        expect(encoded).toBe(expected);
    };

    test("basic functionality", () => {
        encodeEqual("", "");
        encodeEqual("f", "Zg==");
        encodeEqual("fo", "Zm8=");
        encodeEqual("foo", "Zm9v");
        encodeEqual("foob", "Zm9vYg==");
        encodeEqual("fooba", "Zm9vYmE=");
        encodeEqual("foobar", "Zm9vYmFy");

        encodeEqual("🤓", "8J+kkw==");
        encodeEqual("🤓foo🖖", "8J+kk2Zvb/CflpY=");
    });

    test("omit padding", () => {
        const options = { omitPadding: true };

        encodeEqual("", "", options);
        encodeEqual("f", "Zg", options);
        encodeEqual("fo", "Zm8", options);
        encodeEqual("foo", "Zm9v", options);
        encodeEqual("foob", "Zm9vYg", options);
        encodeEqual("fooba", "Zm9vYmE", options);
        encodeEqual("foobar", "Zm9vYmFy", options);

        encodeEqual("🤓", "8J+kkw", options);
        encodeEqual("🤓foo🖖", "8J+kk2Zvb/CflpY", options);
    });

    test("base64url alphabet", () => {
        const options = { alphabet: "base64url" };

        encodeEqual("", "", options);
        encodeEqual("f", "Zg==", options);
        encodeEqual("fo", "Zm8=", options);
        encodeEqual("foo", "Zm9v", options);
        encodeEqual("foob", "Zm9vYg==", options);
        encodeEqual("fooba", "Zm9vYmE=", options);
        encodeEqual("foobar", "Zm9vYmFy", options);

        encodeEqual("🤓", "8J-kkw==", options);
        encodeEqual("🤓foo🖖", "8J-kk2Zvb_CflpY=", options);
    });
});

describe("SharedArrayBuffer", () => {
    // Reading a view onto shared memory snapshots the bytes — rather than encoding them through a pointer into the live
    // buffer that another agent could be writing to. The result must be identical either way.
    const fill = view => {
        for (let i = 0; i < view.length; ++i) view[i] = i * 0x11;
        return view;
    };

    test("matches an equivalent unshared buffer", () => {
        const shared = fill(new Uint8Array(new SharedArrayBuffer(8)));
        const unshared = fill(new Uint8Array(new ArrayBuffer(8)));

        expect(shared.toBase64()).toBe("ABEiM0RVZnc=");
        expect(shared.toBase64()).toBe(unshared.toBase64());
    });

    test("snapshot starts at the view's byteOffset", () => {
        const buffer = new SharedArrayBuffer(8);
        fill(new Uint8Array(buffer));

        // Encoding the wrong window of the buffer would go unnoticed without this — the whole-buffer case above reads
        // from offset 0, where an offset bug in the snapshot is invisible.
        expect(new Uint8Array(buffer, 3, 4).toBase64()).toBe("M0RVZg==");
    });
});
