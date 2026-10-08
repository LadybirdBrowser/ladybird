test("basic functionality", () => {
    let re = /foo/;
    expect(re.exec.length).toBe(1);

    let res = re.exec("foo");
    expect(res.length).toBe(1);
    expect(res[0]).toBe("foo");
    expect(res.groups).toBe(undefined);
    expect(res.index).toBe(0);
});

test("basic unnamed captures", () => {
    let re = /f(o.*)/;
    let res = re.exec("fooooo");

    expect(res.length).toBe(2);
    expect(res[0]).toBe("fooooo");
    expect(res[1]).toBe("ooooo");
    expect(res.groups).toBe(undefined);
    expect(res.index).toBe(0);

    re = /(foo)(bar)?/;
    res = re.exec("foo");

    expect(res.length).toBe(3);
    expect(res[0]).toBe("foo");
    expect(res[1]).toBe("foo");
    expect(res[2]).toBe(undefined);
    expect(res.groups).toBe(undefined);
    expect(res.index).toBe(0);

    re = /(foo)?(bar)/;
    res = re.exec("bar");

    expect(res.length).toBe(3);
    expect(res[0]).toBe("bar");
    expect(res[1]).toBe(undefined);
    expect(res[2]).toBe("bar");
    expect(res.groups).toBe(undefined);
    expect(res.index).toBe(0);
});

test("basic named captures", () => {
    let re = /f(?<os>o.*)/;
    let res = re.exec("fooooo");

    expect(res.length).toBe(2);
    expect(res.index).toBe(0);
    expect(res[0]).toBe("fooooo");
    expect(res[1]).toBe("ooooo");
    expect(res.groups).not.toBe(undefined);
    expect(res.groups.os).toBe("ooooo");
});

test("basic index", () => {
    let re = /foo/;
    let res = re.exec("abcfoo");

    expect(res.length).toBe(1);
    expect(res.index).toBe(3);
    expect(res[0]).toBe("foo");
});

test("basic index with global and initial offset", () => {
    let re = /foo/g;
    re.lastIndex = 2;
    let res = re.exec("abcfoo");

    expect(res.length).toBe(1);
    expect(res.index).toBe(3);
    expect(res[0]).toBe("foo");
});

test("not matching", () => {
    let re = /foo/;
    let res = re.exec("bar");

    expect(res).toBe(null);
});

// Backreference to a group not yet parsed: #6039
test("Future group backreference, #6039", () => {
    let re = /(\3)(\1)(a)/;
    let result = re.exec("cat");
    expect(result.length).toBe(4);
    expect(result[0]).toBe("a");
    expect(result[1]).toBe("");
    expect(result[2]).toBe("");
    expect(result[3]).toBe("a");
    expect(result.index).toBe(1);
});

// #6108
test("optionally seen capture group", () => {
    let rmozilla = /(mozilla)(?:.*? rv:([\w.]+))?/;
    let ua = "mozilla/4.0 (serenityos; x86) libweb+libjs (not khtml, nor gecko) libweb";
    let res = rmozilla.exec(ua);

    expect(res.length).toBe(3);
    expect(res[0]).toBe("mozilla");
    expect(res[1]).toBe("mozilla");
    expect(res[2]).toBeUndefined();
});

// #6131
test("capture group with two '?' qualifiers", () => {
    let res = /()??/.exec("");

    expect(res.length).toBe(2);
    expect(res[0]).toBe("");
    expect(res[1]).toBeUndefined();
});

test("named capture group with two '?' qualifiers", () => {
    let res = /(?<foo>)??/.exec("");

    expect(res.length).toBe(2);
    expect(res[0]).toBe("");
    expect(res[1]).toBeUndefined();
    expect(res.groups.foo).toBeUndefined();
});

// #6042
test("non-greedy brace quantifier", () => {
    let res = /a[a-z]{2,4}?/.exec("abcdefghi");

    expect(res.length).toBe(1);
    expect(res[0]).toBe("abc");
});

// #6208
test("brace quantifier with invalid contents", () => {
    let re = /{{lit-746579221856449}}|<!--{{lit-746579221856449}}-->/;
    let res = re.exec("{{lit-746579221856449}}");

    expect(res.length).toBe(1);
    expect(res[0]).toBe("{{lit-746579221856449}}");
});

test("lazy quantified capture restores the previous iteration when backtracking", () => {
    let res = /^(b+|a){1,2}?bc/.exec("bbc");

    expect(res.length).toBe(2);
    expect(res[0]).toBe("bbc");
    expect(res[1]).toBe("b");
    expect(res.index).toBe(0);
});

test("zero-width quantified captures fall back to the pre-iteration state", () => {
    let res = /(a*)*/.exec("b");

    expect(res.length).toBe(2);
    expect(res[0]).toBe("");
    expect(res[1]).toBeUndefined();
    expect(res.index).toBe(0);
});

// #6256
test("empty character class semantics", () => {
    // Should not match zero-length strings.
    let res = /[]/.exec("");
    expect(res).toBe(null);

    // Inverse form, should match anything.
    res = /[^]/.exec("x");
    expect(res.length).toBe(1);
    expect(res[0]).toBe("x");
});

// #6409
test("undefined match result", () => {
    const r = /foo/;
    r.exec = () => ({});
    expect(r[Symbol.replace]()).toBe("undefined");
});

// Multiline test
test("multiline match", () => {
    let reg = /\s*\/\/.*$/gm;
    let string = `
(
(?:[a-fA-F\d]{1,4}:){7}(?:[a-fA-F\d]{1,4}|:)|                                // 1:2:3:4:5:6:7::  1:2:3:4:5:6:7:8
(?:[a-fA-F\d]{1,4}:){6}(?:(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)(?:\.(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)){3}|:[a-fA-F\d]{1,4}|:)|                         // 1:2:3:4:5:6::    1:2:3:4:5:6::8   1:2:3:4:5:6::8  1:2:3:4:5:6::1.2.3.4
(?:[a-fA-F\d]{1,4}:){5}(?::(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)(?:\.(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)){3}|(:[a-fA-F\d]{1,4}){1,2}|:)|                 // 1:2:3:4:5::      1:2:3:4:5::7:8   1:2:3:4:5::8    1:2:3:4:5::7:1.2.3.4
(?:[a-fA-F\d]{1,4}:){4}(?:(:[a-fA-F\d]{1,4}){0,1}:(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)(?:\.(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)){3}|(:[a-fA-F\d]{1,4}){1,3}|:)| // 1:2:3:4::        1:2:3:4::6:7:8   1:2:3:4::8      1:2:3:4::6:7:1.2.3.4
(?:[a-fA-F\d]{1,4}:){3}(?:(:[a-fA-F\d]{1,4}){0,2}:(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)(?:\.(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)){3}|(:[a-fA-F\d]{1,4}){1,4}|:)| // 1:2:3::          1:2:3::5:6:7:8   1:2:3::8        1:2:3::5:6:7:1.2.3.4
(?:[a-fA-F\d]{1,4}:){2}(?:(:[a-fA-F\d]{1,4}){0,3}:(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)(?:\.(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)){3}|(:[a-fA-F\d]{1,4}){1,5}|:)| // 1:2::            1:2::4:5:6:7:8   1:2::8          1:2::4:5:6:7:1.2.3.4
(?:[a-fA-F\d]{1,4}:){1}(?:(:[a-fA-F\d]{1,4}){0,4}:(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)(?:\.(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)){3}|(:[a-fA-F\d]{1,4}){1,6}|:)| // 1::              1::3:4:5:6:7:8   1::8            1::3:4:5:6:7:1.2.3.4
(?::((?::[a-fA-F\d]{1,4}){0,5}:(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)(?:\.(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)){3}|(?::[a-fA-F\d]{1,4}){1,7}|:))           // ::2:3:4:5:6:7:8  ::2:3:4:5:6:7:8  ::8             ::1.2.3.4
)(%[0-9a-zA-Z]{1,})?                                           // %eth0            %1
`;

    let res = reg.exec(string);
    expect(res.length).toBe(1);
    expect(res[0]).toBe("                                // 1:2:3:4:5:6:7::  1:2:3:4:5:6:7:8");
    expect(res.index).toBe(46);
});

test("multiline stateful match", () => {
    let reg = /\s*\/\/.*$/gm;
    let string = `
(
(?:[a-fA-F\d]{1,4}:){7}(?:[a-fA-F\d]{1,4}|:)|                                // 1:2:3:4:5:6:7::  1:2:3:4:5:6:7:8
(?:[a-fA-F\d]{1,4}:){6}(?:(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)(?:\.(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)){3}|:[a-fA-F\d]{1,4}|:)|                         // 1:2:3:4:5:6::    1:2:3:4:5:6::8   1:2:3:4:5:6::8  1:2:3:4:5:6::1.2.3.4
(?:[a-fA-F\d]{1,4}:){5}(?::(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)(?:\.(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)){3}|(:[a-fA-F\d]{1,4}){1,2}|:)|                 // 1:2:3:4:5::      1:2:3:4:5::7:8   1:2:3:4:5::8    1:2:3:4:5::7:1.2.3.4
(?:[a-fA-F\d]{1,4}:){4}(?:(:[a-fA-F\d]{1,4}){0,1}:(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)(?:\.(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)){3}|(:[a-fA-F\d]{1,4}){1,3}|:)| // 1:2:3:4::        1:2:3:4::6:7:8   1:2:3:4::8      1:2:3:4::6:7:1.2.3.4
(?:[a-fA-F\d]{1,4}:){3}(?:(:[a-fA-F\d]{1,4}){0,2}:(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)(?:\.(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)){3}|(:[a-fA-F\d]{1,4}){1,4}|:)| // 1:2:3::          1:2:3::5:6:7:8   1:2:3::8        1:2:3::5:6:7:1.2.3.4
(?:[a-fA-F\d]{1,4}:){2}(?:(:[a-fA-F\d]{1,4}){0,3}:(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)(?:\.(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)){3}|(:[a-fA-F\d]{1,4}){1,5}|:)| // 1:2::            1:2::4:5:6:7:8   1:2::8          1:2::4:5:6:7:1.2.3.4
(?:[a-fA-F\d]{1,4}:){1}(?:(:[a-fA-F\d]{1,4}){0,4}:(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)(?:\.(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)){3}|(:[a-fA-F\d]{1,4}){1,6}|:)| // 1::              1::3:4:5:6:7:8   1::8            1::3:4:5:6:7:1.2.3.4
(?::((?::[a-fA-F\d]{1,4}){0,5}:(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)(?:\.(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]\d|\d)){3}|(?::[a-fA-F\d]{1,4}){1,7}|:))           // ::2:3:4:5:6:7:8  ::2:3:4:5:6:7:8  ::8             ::1.2.3.4
)(%[0-9a-zA-Z]{1,})?                                           // %eth0            %1
`;

    let res = reg.exec(string);
    expect(res.length).toBe(1);
    expect(res[0]).toBe("                                // 1:2:3:4:5:6:7::  1:2:3:4:5:6:7:8");
    expect(res.index).toBe(46);

    res = reg.exec(string);
    expect(res.length).toBe(1);
    expect(res[0]).toBe(
        "                         // 1:2:3:4:5:6::    1:2:3:4:5:6::8   1:2:3:4:5:6::8  1:2:3:4:5:6::1.2.3.4"
    );
    expect(res.index).toBe(231);
});

test("string coercion", () => {
    let result = /1/.exec(1);
    expect(result.length).toBe(1);
    expect(result[0]).toBe("1");
    expect(result.index).toBe(0);
});

test("cached UTF-16 code point length", () => {
    // This exercises a regression where we incorrectly cached the code point length of the `match` string,
    // causing subsequent code point lookups on that string to be incorrect.
    const regex = /\p{Emoji_Presentation}/u;

    let result = regex.exec("😀");
    let match = result[0];

    result = regex.exec(match);
    match = result[0];

    expect(match.codePointAt(0)).toBe(0x1f600);
});

test("UTF-16 captures", () => {
    let result = /(\ud83d\ude00)(\ud83d)(\ude00)/.exec("😀😀");

    expect(result).not.toBe(null);
    expect(result[0]).toBe("😀😀");
    expect(result[1]).toBe("😀");
    expect(result[2]).toBe("\ud83d");
    expect(result[3]).toBe("\ude00");
});

test("named groups source order", () => {
    // Test that named groups appear in source order, not match order
    let re = /(?<y>a)(?<x>a)|(?<x>b)(?<y>b)/;

    let result1 = re.exec("aa");
    expect(Object.keys(result1.groups)).toEqual(["y", "x"]);
    expect(result1.groups.y).toBe("a");
    expect(result1.groups.x).toBe("a");

    let result2 = re.exec("bb");
    expect(Object.keys(result2.groups)).toEqual(["y", "x"]);
    expect(result2.groups.y).toBe("b");
    expect(result2.groups.x).toBe("b");
});

test("named groups all present in groups object", () => {
    // Test that all named groups appear in groups object, even unmatched ones
    let re = /(?<fst>.)|(?<snd>.)/u;

    let result = re.exec("abcd");
    expect(Object.getOwnPropertyNames(result.groups)).toEqual(["fst", "snd"]);
    expect(result.groups.fst).toBe("a");
    expect(result.groups.snd).toBe(undefined);
});

test("named groups with hasIndices flag", () => {
    // Test that indices.groups also contains all named groups in source order
    let re = /(?<fst>.)|(?<snd>.)/du;

    let result = re.exec("abcd");
    expect(Object.getOwnPropertyNames(result.indices.groups)).toEqual(["fst", "snd"]);
    expect(result.indices.groups.fst).toEqual([0, 1]);
    expect(result.indices.groups.snd).toBe(undefined);
});

test("complex named groups ordering", () => {
    // Test multiple groups in different order
    let re = /(?<third>c)|(?<first>a)|(?<second>b)/;

    let result1 = re.exec("a");
    expect(Object.keys(result1.groups)).toEqual(["third", "first", "second"]);
    expect(result1.groups.third).toBe(undefined);
    expect(result1.groups.first).toBe("a");
    expect(result1.groups.second).toBe(undefined);

    let result2 = re.exec("b");
    expect(Object.keys(result2.groups)).toEqual(["third", "first", "second"]);
    expect(result2.groups.third).toBe(undefined);
    expect(result2.groups.first).toBe(undefined);
    expect(result2.groups.second).toBe("b");

    let result3 = re.exec("c");
    expect(Object.keys(result3.groups)).toEqual(["third", "first", "second"]);
    expect(result3.groups.third).toBe("c");
    expect(result3.groups.first).toBe(undefined);
    expect(result3.groups.second).toBe(undefined);
});

test("forward references to named groups", () => {
    // Self-reference inside group
    let result1 = /(?<a>\k<a>\w)../.exec("bab");
    expect(result1).not.toBe(null);
    expect(result1[0]).toBe("bab");
    expect(result1[1]).toBe("b");
    expect(result1.groups.a).toBe("b");

    // Reference before group definition
    let result2 = /\k<a>(?<a>b)\w\k<a>/.exec("bab");
    expect(result2).not.toBe(null);
    expect(result2[0]).toBe("bab");
    expect(result2[1]).toBe("b");
    expect(result2.groups.a).toBe("b");

    let result3 = /(?<b>b)\k<a>(?<a>a)\k<b>/.exec("bab");
    expect(result3).not.toBe(null);
    expect(result3[0]).toBe("bab");
    expect(result3[1]).toBe("b");
    expect(result3[2]).toBe("a");
    expect(result3.groups.a).toBe("a");
    expect(result3.groups.b).toBe("b");

    // Backward reference
    let result4 = /(?<a>a)(?<b>b)\k<a>/.exec("aba");
    expect(result4).not.toBe(null);
    expect(result4[0]).toBe("aba");
    expect(result4.groups.a).toBe("a");
    expect(result4.groups.b).toBe("b");

    // Mixed forward/backward with alternation
    let result5 = /(?<a>a)(?<b>b)\k<a>|(?<c>c)/.exec("aba");
    expect(result5).not.toBe(null);
    expect(result5.groups.a).toBe("a");
    expect(result5.groups.b).toBe("b");
    expect(result5.groups.c).toBe(undefined);
});

test("invalid named group references", () => {
    expect(() => {
        new RegExp("(?<a>x)\\k<nonexistent>");
    }).toThrow();
});

test("pathological backtracking pattern should match", () => {
    // This pattern requires many backtracking steps but should still match.
    let result = /(a?){17}a{17}/.exec("aaaaaaaaaaaaaaaaa");
    expect(result).not.toBe(null);
    expect(result[0].length).toBe(17);
});

test("alternation uses leftmost-first semantics", () => {
    // ECMAScript requires the first alternative to win, not the longest.
    let result = /a|ab/.exec("ab");
    expect(result).not.toBe(null);
    expect(result[0]).toBe("a");

    expect("ab".replace(/a|ab/g, "X")).toBe("Xb");
});

test("case-insensitive Unicode matching of astral characters", () => {
    // U+10400 (Deseret Capital Letter Long I) should case-fold to U+10428.
    let result = /\u{10400}/iu.exec("\u{10428}");
    expect(result).not.toBe(null);
    expect(result[0]).toBe("\u{10428}");

    // And vice versa.
    result = /\u{10428}/iu.exec("\u{10400}");
    expect(result).not.toBe(null);
    expect(result[0]).toBe("\u{10400}");
});

test("character classes with builtin classes", () => {
    const firstMatch = (regexp, string) => {
        const match = regexp.exec(string);
        return match === null ? null : [match.index, match[0]];
    };

    expect(firstMatch(/[^\s"]+/, '  \t"abc" def')).toEqual([4, "abc"]);
    expect(firstMatch(/[^\s\/>"'=]+/, ' class="toggle"')).toEqual([1, "class"]);
    expect(firstMatch(/[^\w-]+/, "12ab-CD_ef!?")).toEqual([10, "!?"]);
    expect(firstMatch(/[\W\d]+/, "12ab")).toEqual([0, "12"]);
    expect(firstMatch(/[^\W_]+/, "__ab_")).toEqual([2, "ab"]);
    expect(firstMatch(/[^\S]+/, "a 　﻿b")).toEqual([1, " 　﻿"]);
    expect(firstMatch(/[^\D]+/, "ab12c")).toEqual([2, "12"]);
    expect(firstMatch(/[\s\S]+/, "a\nb")).toEqual([0, "a\nb"]);
    expect(firstMatch(/[^\w\s]+/, "ab  -+")).toEqual([4, "-+"]);
    expect(firstMatch(/a[^\s]*?b/, "a b axyb")).toEqual([4, "axyb"]);
    expect(firstMatch(/[^\s]{2,3}/, "a bcde")).toEqual([2, "bcd"]);

    // Without the u flag, a class matches the code units of a surrogate pair one at a time.
    expect(firstMatch(/[^\s\uD83D]+/, "😀 x")).toEqual([1, "\uDE00"]);
    expect(firstMatch(/[\D]/, "😀")).toEqual([0, "\uD83D"]);

    // Case-insensitive classes.
    expect(firstMatch(/[^\sa-c]+/i, "ABC xyAbz")).toEqual([4, "xy"]);
    expect(firstMatch(/[^\w]+/i, "ſK")).toEqual([0, "ſK"]);
    expect(firstMatch(/[\w-]+/i, "ſ-k")).toEqual([1, "-k"]);
    expect(firstMatch(/[\s-]+/i, "ab - c")).toEqual([2, " - "]);
});

test("alternatives that start with literal characters", () => {
    const tokenizer = /<!--([\s\S]*?)-->|<(\?[^>]*)>|<\/([a-z]+)>|<([a-z]+)>|([^<]+|<)/g;
    const tokens = [];
    let match;
    while ((match = tokenizer.exec("a<b>c<!--d--></b><?e>< f")))
        tokens.push(match.findIndex((capture, index) => index > 0 && capture !== undefined) + ":" + match[0]);
    expect(tokens).toEqual(["5:a", "4:<b>", "5:c", "1:<!--d-->", "3:</b>", "2:<?e>", "5:<", "5: f"]);

    expect(/ab|ac|(a)d/.exec("ad")[1]).toBe("a");
    expect(/(?:x|(y))z|yw/.exec("yw")).toEqual(["yw", undefined]);
    expect(/a(?:bc|bd)|ab/i.exec("ABD")[0]).toBe("ABD");
    expect(/(?:[a-c]x|[a-c]y)/.exec("by")[0]).toBe("by");
});

test("modifiers end with their group after backtracking into it", () => {
    expect(/(?i:[^\s"]+)x/.exec('x"yX')).toBeNull();
    expect(/(?i:[^\s"]+)x/.exec("xyX")).toBeNull();
    expect(/(?i:[^\s"]+)x/.exec("xyx")[0]).toBe("xyx");
    expect(/(?i:a+)b/.exec("AaaAB")).toBeNull();
    expect(/(?i:a+)b/.exec("AaaAb")[0]).toBe("AaaAb");
    expect(/(?-i:a+)b/i.exec("aaB")[0]).toBe("aaB");
    expect(/(?-i:a+)b/i.exec("aAb")).toBeNull();
    expect(/(?i:a(?-i:b)+)c/.exec("Abbbc")[0]).toBe("Abbbc");
    expect(/(?i:a(?-i:b)+)c/.exec("AbbbC")).toBeNull();
    expect(/(?=(?i:a+))a/.exec("Aa")[0]).toBe("a");
});

test("lookarounds restore state without outer alternatives", () => {
    expect(/^(?!(a)b)a$/.exec("a")).toEqual(["a", undefined]);
    expect(/^(?!(?=(a))b)a$/.exec("a")).toEqual(["a", undefined]);
    expect(/^(?=(a))a$/.exec("a")).toEqual(["a", "a"]);
    expect(/^(?!(?i:a)b)a$/.exec("a")[0]).toBe("a");
    expect(/^(?=(?i:a))a$/.exec("a")[0]).toBe("a");
    expect(/^(?!(?=(?i:a))b)a$/.exec("a")[0]).toBe("a");
});
