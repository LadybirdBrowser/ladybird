// https://github.com/tc39/test262/blob/main/test/staging/sm/RegExp/sticky.js
describe("RegExp sticky flag", () => {
    function testSticky(re, text, expectations) {
        expect(expectations.length).toBe(text.length + 1);

        for (let i = 0; i < expectations.length; i++) {
            const result = expectations[i];

            re.lastIndex = i;
            const match = re.exec(text);
            if (result === null) {
                expect(re.lastIndex).toBe(0);
                expect(match).toBe(null);
            } else {
                expect(re.lastIndex).toBe(result.lastIndex);
                expect(match !== null).toBe(true);
                expect(match.length).toBe(result.matches.length);
                for (let j = 0; j < result.matches.length; j++) expect(match[j]).toBe(result.matches[j]);
                expect(match.index).toBe(result.index);
            }
        }
    }

    test("simple text", () => {
        testSticky(/bc/y, "abcabd", [null, { lastIndex: 3, matches: ["bc"], index: 1 }, null, null, null, null, null]);
    });

    test("complex pattern", () => {
        testSticky(/bc|c|d/y, "abcabd", [
            null,
            { lastIndex: 3, matches: ["bc"], index: 1 },
            { lastIndex: 3, matches: ["c"], index: 2 },
            null,
            null,
            { lastIndex: 6, matches: ["d"], index: 5 },
            null,
        ]);
    });

    test("greedy quantifier", () => {
        testSticky(/.*(bc|c|d)/y, "abcabd", [
            { lastIndex: 6, matches: ["abcabd", "d"], index: 0 },
            { lastIndex: 6, matches: ["bcabd", "d"], index: 1 },
            { lastIndex: 6, matches: ["cabd", "d"], index: 2 },
            { lastIndex: 6, matches: ["abd", "d"], index: 3 },
            { lastIndex: 6, matches: ["bd", "d"], index: 4 },
            { lastIndex: 6, matches: ["d", "d"], index: 5 },
            null,
        ]);
    });

    test("non-greedy quantifier", () => {
        testSticky(/.*?(bc|c|d)/y, "abcabd", [
            { lastIndex: 3, matches: ["abc", "bc"], index: 0 },
            { lastIndex: 3, matches: ["bc", "bc"], index: 1 },
            { lastIndex: 3, matches: ["c", "c"], index: 2 },
            { lastIndex: 6, matches: ["abd", "d"], index: 3 },
            { lastIndex: 6, matches: ["bd", "d"], index: 4 },
            { lastIndex: 6, matches: ["d", "d"], index: 5 },
            null,
        ]);
    });

    test("alternation with greedy", () => {
        testSticky(/(bc|.*c|d)/y, "abcabd", [
            { lastIndex: 3, matches: ["abc", "abc"], index: 0 },
            { lastIndex: 3, matches: ["bc", "bc"], index: 1 },
            { lastIndex: 3, matches: ["c", "c"], index: 2 },
            null,
            null,
            { lastIndex: 6, matches: ["d", "d"], index: 5 },
            null,
        ]);
    });

    test("^ assertion", () => {
        testSticky(/^/y, "abcabc", [{ lastIndex: 0, matches: [""], index: 0 }, null, null, null, null, null, null]);
    });

    test("^ assertion with multiline", () => {
        testSticky(/^a/my, "abc\nabc", [
            { lastIndex: 1, matches: ["a"], index: 0 },
            null,
            null,
            null,
            { lastIndex: 5, matches: ["a"], index: 4 },
            null,
            null,
            null,
        ]);
    });

    test("\\b assertion", () => {
        testSticky(/\b/y, "abc bc", [
            { lastIndex: 0, matches: [""], index: 0 },
            null,
            null,
            { lastIndex: 3, matches: [""], index: 3 },
            { lastIndex: 4, matches: [""], index: 4 },
            null,
            { lastIndex: 6, matches: [""], index: 6 },
        ]);
    });

    test("\\B assertion", () => {
        testSticky(/\B/y, "abc bc", [
            null,
            { lastIndex: 1, matches: [""], index: 1 },
            { lastIndex: 2, matches: [""], index: 2 },
            null,
            null,
            { lastIndex: 5, matches: [""], index: 5 },
            null,
        ]);
    });
});
