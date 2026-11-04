test("numeric literal keys in binding patterns use the number's string value", () => {
    const {
        0x10: hex,
        0o10: octal,
        0b10: binary,
        1_0: separated,
        1e3: exponent,
        1.5: fraction,
    } = {
        16: "hex",
        8: "octal",
        2: "binary",
        10: "separated",
        1000: "exponent",
        1.5: "fraction",
    };
    expect(hex).toBe("hex");
    expect(octal).toBe("octal");
    expect(binary).toBe("binary");
    expect(separated).toBe("separated");
    expect(exponent).toBe("exponent");
    expect(fraction).toBe("fraction");
});

test("BigInt literal keys in binding patterns use the BigInt's string value", () => {
    const {
        0x10n: hex,
        0o10n: octal,
        0b10n: binary,
        1_0n: separated,
        0x1_0n: hexSeparated,
    } = {
        16: "hex",
        8: "octal",
        2: "binary",
        10: "separated",
    };
    expect(hex).toBe("hex");
    expect(octal).toBe("octal");
    expect(binary).toBe("binary");
    expect(separated).toBe("separated");
    expect(hexSeparated).toBe("hex");
});

test("literal keys in assignment patterns and parameters", () => {
    let a, b;
    ({ 0x10n: a, 0x10: b } = { 16: "ok" });
    expect(a).toBe("ok");
    expect(b).toBe("ok");

    function f({ 0b11n: c }) {
        return c;
    }
    expect(f({ 3: "ok" })).toBe("ok");
});

test("BigInt literal keys need an alias", () => {
    expect("const { 1n } = {};").not.toEval();
    expect("({ 1n } = {});").not.toEval();
});
