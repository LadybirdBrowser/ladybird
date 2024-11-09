test("invariants", () => {
    expect(String.prototype.replaceAll).toHaveLength(2);
});

test("error cases", () => {
    [null, undefined].forEach(value => {
        expect(() => {
            value.replace("", "");
        }).toThrow(TypeError);
    });

    expect(() => {
        "".replaceAll(/abc/, "");
    }).toThrow(TypeError);
});

test("basic string replacement", () => {
    expect("".replaceAll("", "")).toBe("");
    expect("".replaceAll("a", "")).toBe("");
    expect("".replaceAll("", "a")).toBe("a");
    expect("abc".replaceAll("", "x")).toBe("xaxbxcx");

    expect("a".replaceAll("a", "")).toBe("");
    expect("a".replaceAll("a", "b")).toBe("b");
    expect("aa".replaceAll("a", "b")).toBe("bb");
    expect("ca".replaceAll("a", "b")).toBe("cb");
    expect("aca".replaceAll("a", "b")).toBe("bcb");
    expect("aca".replaceAll("ca", "b")).toBe("ab");
    expect("aca".replaceAll("ac", "b")).toBe("ba");
});

test("convertible string replacement", () => {
    expect("1223".replaceAll(2, "x")).toBe("1xx3");
    expect("1223".replaceAll("2", 4)).toBe("1443");
    expect("1223".replaceAll(2, 4)).toBe("1443");
});

test("functional string replacement", () => {
    expect(
        "aba".replaceAll("a", function () {
            return "c";
        })
    ).toBe("cbc");
    expect("aba".replaceAll("a", () => "c")).toBe("cbc");

    expect(
        "aba".replaceAll("a", (search, position, string) => {
            expect(search).toBe("a");
            expect(position <= 2).toBeTrue();
            expect(string).toBe("aba");
            return "x";
        })
    ).toBe("xbx");
});

test("basic regex replacement", () => {
    expect("".replaceAll(/a/g, "")).toBe("");
    expect("a".replaceAll(/a/g, "")).toBe("");

    expect("abc123def".replaceAll(/\D/g, "*")).toBe("***123***");
    expect("123abc456".replaceAll(/\D/g, "*")).toBe("123***456");

    expect("aaab a a aac".replaceAll("aa", "z")).toBe("zab a a zc");
    expect("aaab a a aac".replaceAll("aa", "a")).toBe("aab a a ac");
    expect("aaab a a aac".replaceAll("a", "a")).toBe("aaab a a aac");
    expect("aaab a a aac".replaceAll("a", "z")).toBe("zzzb z z zzc");
});

test("functional regex replacement", () => {
    expect(
        "a".replace(/a/g, function () {
            return "b";
        })
    ).toBe("b");
    expect("a".replace(/a/g, () => "b")).toBe("b");

    expect(
        "abc".replace(/\D/g, (matched, position, string) => {
            expect(matched).toBe(string[position]);
            expect(position <= 2).toBeTrue();
            expect(string).toBe("abc");
            return "x";
        })
    ).toBe("xxx");

    expect(
        "abc".replace(/(\D)/g, (matched, capture1, position, string) => {
            expect(matched).toBe(string[position]);
            expect(capture1).toBe(string[position]);
            expect(position <= 2).toBeTrue();
            expect(string).toBe("abc");
            return "x";
        })
    ).toBe("xxx");

    expect(
        "abcd".replace(/(\D)b(\D)/g, (matched, capture1, capture2, position, string) => {
            expect(matched).toBe("abc");
            expect(capture1).toBe("a");
            expect(capture2).toBe("c");
            expect(position).toBe(0);
            expect(string).toBe("abcd");
            return "x";
        })
    ).toBe("xd");
});

test("replacement value is evaluated before searching the source string", () => {
    var calls = 0;
    var replaceValue = {
        toString: function () {
            calls += 1;
            return "b";
        },
    };

    var newString = "".replaceAll("a", replaceValue);
    expect(newString).toBe("");
    expect(calls).toBe(1);

    newString = "".replaceAll(/a/g, replaceValue);
    expect(newString).toBe("");
    expect(calls).toBe(2);
});

test("search value is coerced to a string", () => {
    var calls = 0;
    var coerced;

    var searchValue = {
        toString: function () {
            calls += 1;
            return "x";
        },
    };

    var replaceValue = function (matched) {
        coerced = matched;
        return "abc";
    };

    var newString = "x".replaceAll(searchValue, replaceValue);
    expect(newString).toBe("abc");
    expect(coerced).toBe("x");
});

test("UTF-16", () => {
    expect("😀".replaceAll("😀", "")).toBe("");
    expect("😀".replaceAll("\ud83d", "")).toBe("\ude00");
    expect("😀".replaceAll("\ude00", "")).toBe("\ud83d");
    expect("😀😀😀".replaceAll("\ud83d", "")).toBe("\ude00\ude00\ude00");
    expect("😀😀😀".replaceAll("\ude00", "")).toBe("\ud83d\ud83d\ud83d");

    expect("😀".replaceAll(/\ud83d/g, "")).toBe("\ude00");
    expect("😀".replaceAll(/\ude00/g, "")).toBe("\ud83d");
    expect("😀".replaceAll(/\ud83d\ude00/g, "")).toBe("");
    expect("😀😀😀".replaceAll(/\ud83d/g, "")).toBe("\ude00\ude00\ude00");
    expect("😀😀😀".replaceAll(/\ude00/g, "")).toBe("\ud83d\ud83d\ud83d");
    expect("😀😀😀".replaceAll(/\ud83d\ude00/g, "")).toBe("");

    expect("😀".replaceAll(/\ud83d/gu, "")).toBe("😀");
    expect("😀".replaceAll(/\ude00/gu, "")).toBe("😀");
    expect("😀".replaceAll(/\ud83d\ude00/gu, "")).toBe("");
    expect("😀😀😀".replaceAll(/\ud83d/gu, "")).toBe("😀😀😀");
    expect("😀😀😀".replaceAll(/\ude00/gu, "")).toBe("😀😀😀");
    expect("😀😀😀".replaceAll(/\ude00/gu, "")).toBe("😀😀😀");
    expect("😀😀😀".replaceAll(/\ud83d\ude00/gu, "")).toBe("");
});
