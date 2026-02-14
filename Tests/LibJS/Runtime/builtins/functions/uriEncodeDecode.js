test("encodeURI", () => {
    [
        ["шеллы", "%D1%88%D0%B5%D0%BB%D0%BB%D1%8B"],
        [";,/?:@&=+$#", ";,/?:@&=+$#"],
        ["-_.!~*'()", "-_.!~*'()"],
        ["ABC abc 123", "ABC%20abc%20123"],
    ].forEach(test => {
        expect(encodeURI(test[0])).toBe(test[1]);
    });
});

test("decodeURI", () => {
    [
        ["%D1%88%D0%B5%D0%BB%D0%BB%D1%8B", "шеллы"],
        [";,/?:@&=+$#", ";,/?:@&=+$#"],
        ["-_.!~*'()", "-_.!~*'()"],
        ["ABC%20abc%20123", "ABC abc 123"],
    ].forEach(test => {
        expect(decodeURI(test[0])).toBe(test[1]);
    });
});

test("decodeURI exception", () => {
    ["%", "%a", "%gh", "%%%"].forEach(test => {
        expect(() => {
            decodeURI(test);
        }).toThrowWithMessage(URIError, "URI malformed");
    });
});

test("encodeURIComponent", () => {
    [
        ["шеллы", "%D1%88%D0%B5%D0%BB%D0%BB%D1%8B"],
        [";,/?:@&=+$#", "%3B%2C%2F%3F%3A%40%26%3D%2B%24%23"],
        ["-_.!~*'()", "-_.!~*'()"],
        ["ABC abc 123", "ABC%20abc%20123"],
    ].forEach(test => {
        expect(encodeURIComponent(test[0])).toBe(test[1]);
    });
});

test("decodeURIComponent", () => {
    [
        ["%D1%88%D0%B5%D0%BB%D0%BB%D1%8B", "шеллы"],
        ["%3B%2C%2F%3F%3A%40%26%3D%2B%24%23", ";,/?:@&=+$#"],
        ["-_.!~*'()", "-_.!~*'()"],
        ["ABC%20abc%20123", "ABC abc 123"],
    ].forEach(test => {
        expect(decodeURIComponent(test[0])).toBe(test[1]);
    });
});

test("decodeURIComponent exception", () => {
    ["%", "%a", "%gh", "%%%"].forEach(test => {
        expect(() => {
            decodeURIComponent(test);
        }).toThrowWithMessage(URIError, "URI malformed");
    });
});

test("decodeURIComponent invalid UTF-8 sequences", () => {
    [
        "%ED%BF%BF", // surrogate code point (U+DFFF)
        "%C0%AF", // overlong encoding
        "%ED%7F%BF", // invalid continuation byte
        "%ED%BF", // incomplete 3-byte sequence
        "%F4%90%80%80", // code point beyond U+10FFFF
    ].forEach(test => {
        expect(() => {
            decodeURIComponent(test);
        }).toThrowWithMessage(URIError, "URI malformed");
    });
});
