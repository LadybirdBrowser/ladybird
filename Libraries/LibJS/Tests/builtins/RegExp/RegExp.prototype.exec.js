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
