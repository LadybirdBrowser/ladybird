test("escape", () => {
    [
        ["abc123", "abc123"],
        ["äöü", "%E4%F6%FC"],
        ["ć", "%u0107"],
        ["@*_+-./", "@*_+-./"],
        ["\ud834\udf06", "%uD834%uDF06"],
    ].forEach(test => {
        expect(escape(test[0])).toBe(test[1]);
    });
});

test("unescape", () => {
    [
        ["abc123", "abc123"],
        ["%E4%F6%FC", "äöü"],
        ["%u0107", "ć"],
        ["@*_+-./", "@*_+-./"],
    ].forEach(test => {
        expect(unescape(test[0])).toBe(test[1]);
    });
});
