// Characters in Unicode's ID_Start but not XID_Start must be accepted as
// identifier-start characters per the ECMAScript specification.
// https://tc39.es/ecma262/#sec-identifier-names

test("U+0E33 THAI CHARACTER SARA AM as object property key", () => {
    const obj = { ำ: 42 };
    expect(obj["ำ"]).toBe(42);
    expect(obj.ำ).toBe(42);
});

test("U+0E33 THAI CHARACTER SARA AM as variable name", () => {
    const ำ = 99;
    expect(ำ).toBe(99);
});

test("U+0EB3 LAO VOWEL SIGN AM as object property key", () => {
    const obj = { ຳ: 7 };
    expect(obj["ຳ"]).toBe(7);
    expect(obj.ຳ).toBe(7);
});

test("U+0EB3 LAO VOWEL SIGN AM as variable name", () => {
    const ຳ = 13;
    expect(ຳ).toBe(13);
});

test("U+FF9E HALFWIDTH KATAKANA VOICED SOUND MARK as identifier start", () => {
    const obj = { ﾞ: 1 };
    expect(obj["ﾞ"]).toBe(1);
});

test("U+FF9F HALFWIDTH KATAKANA SEMI-VOICED SOUND MARK as identifier start", () => {
    const obj = { ﾟ: 2 };
    expect(obj["ﾟ"]).toBe(2);
});
