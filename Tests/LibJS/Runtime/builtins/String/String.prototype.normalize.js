test("Function length is 0", () => {
    expect(String.prototype.normalize).toHaveLength(0);
});

test("Function name is normalize", () => {
    expect(String.prototype.normalize.name).toBe("normalize");
});

test("Type is function", () => {
    expect(typeof String.prototype.normalize).toBe("function");
});

test("Invalid form throws", () => {
    expect(() => "foo".normalize("bar")).toThrowWithMessage(
        RangeError,
        "The normalization form must be one of NFC, NFD, NFKC, NFKD. Got 'bar'"
    );
    expect(() => "foo".normalize("NFC1")).toThrowWithMessage(
        RangeError,
        "The normalization form must be one of NFC, NFD, NFKC, NFKD. Got 'NFC1'"
    );
    expect(() => "foo".normalize(null)).toThrowWithMessage(
        RangeError,
        "The normalization form must be one of NFC, NFD, NFKC, NFKD. Got 'null'"
    );
});

test("Invalid object throws", () => {
    expect(() => String.prototype.normalize.call(undefined)).toThrowWithMessage(
        TypeError,
        "undefined cannot be converted to an object"
    );
    expect(() => String.prototype.normalize.call(null)).toThrowWithMessage(
        TypeError,
        "null cannot be converted to an object"
    );
});

test("Normalization works", () => {
    var s = "\u1E9B\u0323";

    expect(s.normalize("NFC")).toBe("\u1E9B\u0323");
    expect(s.normalize("NFD")).toBe("\u017F\u0323\u0307");
    expect(s.normalize("NFKC")).toBe("\u1E69");
    expect(s.normalize("NFKD")).toBe("\u0073\u0323\u0307");
});

test("Default parameter is NFC", () => {
    var s = "\u1E9B\u0323";

    expect(s.normalize("NFC")).toBe(s.normalize());
    expect(s.normalize("NFC")).toBe(s.normalize(undefined));
});
