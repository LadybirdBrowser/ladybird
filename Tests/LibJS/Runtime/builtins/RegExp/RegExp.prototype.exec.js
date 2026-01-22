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
