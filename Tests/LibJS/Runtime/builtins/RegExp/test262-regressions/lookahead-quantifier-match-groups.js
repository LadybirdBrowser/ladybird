// https://github.com/tc39/test262/tree/main/test/built-ins/RegExp/lookahead-quantifier-match-groups.js
describe("RegExp lookahead with quantifiers and capture groups", () => {
    test("0-length matches update captures list correctly based on quantifier", () => {
        expect("abc".match(/(?:(?=(abc)))a/)).toEqual(["a", "abc"]);
        expect("abc".match(/(?:(?=(abc)))?a/)).toEqual(["a", undefined]);
        expect("abc".match(/(?:(?=(abc))){1,1}a/)).toEqual(["a", "abc"]);
        expect("abc".match(/(?:(?=(abc))){0,1}a/)).toEqual(["a", undefined]);
    });
});
