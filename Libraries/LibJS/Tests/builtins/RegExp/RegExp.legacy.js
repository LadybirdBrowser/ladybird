// NOTE: This file came from https://github.com/v8/v8/blob/main/test/mjsunit/regexp-static.js
// and was modified to match LibJS test framework.

test("basic functionality", () => {
    var re = /((\d+)\.(\d+))/;
    var s = "abc123.456def";
    re.exec(s);

    expect(RegExp.input).toBe(s);
    expect(RegExp["$_"]).toBe(s);
    expect(RegExp.lastMatch).toBe("123.456");
    expect(RegExp["$&"]).toBe("123.456");
    expect(RegExp.lastParen).toBe("456");
    expect(RegExp["$+"]).toBe("456");
    expect(RegExp.leftContext).toBe("abc");
    expect(RegExp["$`"]).toBe("abc");
    expect(RegExp.rightContext).toBe("def");
    expect(RegExp["$'"]).toBe("def");

    expect(RegExp.$1).toBe("123.456");
    expect(RegExp.$2).toBe("123");
    expect(RegExp.$3).toBe("456");
    expect(RegExp.$4).toBe("");
    expect(RegExp.$5).toBe("");
    expect(RegExp.$6).toBe("");
    expect(RegExp.$7).toBe("");
    expect(RegExp.$8).toBe("");
    expect(RegExp.$9).toBe("");
});

test("They should be read only", () => {
    RegExp["$1"] = "fisk";
    expect(RegExp.$1).toBe("123.456");

    RegExp.input = Number();
    expect(typeof RegExp.input).toBe(typeof String());
});

test("They should be read only (strict mode)", () => {
    "use strict";

    try {
        RegExp["$1"] = "fisk";
        expect(true).toBeFalse(); // SHOULD NOT REACH HERE
    } catch (e) {
        expect(String(e)).toBe("TypeError: Cannot set property '$1' of [object RegExpConstructor]");
        expect(RegExp.$1).toBe("123.456");
    }

    RegExp.input = Number();
    expect(typeof RegExp.input).toBe(typeof String());
});

test("the original accessor vs the alias accessor", () => {
    expect(Object.getOwnPropertyDescriptor(RegExp, "input").get).not.toBe(
        Object.getOwnPropertyDescriptor(RegExp, "$_").get
    );
    expect(Object.getOwnPropertyDescriptor(RegExp, "input").set).not.toBe(
        Object.getOwnPropertyDescriptor(RegExp, "$_").set
    );
    expect(Object.getOwnPropertyDescriptor(RegExp, "lastMatch").get).not.toBe(
        Object.getOwnPropertyDescriptor(RegExp, "$&").get
    );
    expect(Object.getOwnPropertyDescriptor(RegExp, "lastParen").get).not.toBe(
        Object.getOwnPropertyDescriptor(RegExp, "$+").get
    );
    expect(Object.getOwnPropertyDescriptor(RegExp, "leftContext").get).not.toBe(
        Object.getOwnPropertyDescriptor(RegExp, "$`").get
    );
    expect(Object.getOwnPropertyDescriptor(RegExp, "rightContext").get).not.toBe(
        Object.getOwnPropertyDescriptor(RegExp, "$'").get
    );
});

test("match should all behave as if exec were called.", () => {
    var re = /((\d+)\.(\d+))/;
    var s = "ghi789.012jkl";
    s.match(re);

    expect(RegExp.input).toBe(s);
    expect(RegExp["$_"]).toBe(s);
    expect(RegExp.lastMatch).toBe("789.012");
    expect(RegExp["$&"]).toBe("789.012");
    expect(RegExp.lastParen).toBe("012");
    expect(RegExp["$+"]).toBe("012");
    expect(RegExp.leftContext).toBe("ghi");
    expect(RegExp["$`"]).toBe("ghi");
    expect(RegExp.rightContext).toBe("jkl");
    expect(RegExp["$'"]).toBe("jkl");

    expect(RegExp.$1).toBe("789.012");
    expect(RegExp.$2).toBe("789");
    expect(RegExp.$3).toBe("012");
    expect(RegExp.$4).toBe("");
    expect(RegExp.$5).toBe("");
    expect(RegExp.$6).toBe("");
    expect(RegExp.$7).toBe("");
    expect(RegExp.$8).toBe("");
    expect(RegExp.$9).toBe("");
});

test("replace should all behave as if exec were called.", () => {
    var re = /((\d+)\.(\d+))/;
    var s = "abc123.456def";
    s.replace(re, "whocares");

    expect(RegExp.input).toBe(s);
    expect(RegExp["$_"]).toBe(s);
    expect(RegExp.lastMatch).toBe("123.456");
    expect(RegExp["$&"]).toBe("123.456");
    expect(RegExp.lastParen).toBe("456");
    expect(RegExp["$+"]).toBe("456");
    expect(RegExp.leftContext).toBe("abc");
    expect(RegExp["$`"]).toBe("abc");
    expect(RegExp.rightContext).toBe("def");
    expect(RegExp["$'"]).toBe("def");

    expect(RegExp.$1).toBe("123.456");
    expect(RegExp.$2).toBe("123");
    expect(RegExp.$3).toBe("456");
    expect(RegExp.$4).toBe("");
    expect(RegExp.$5).toBe("");
    expect(RegExp.$6).toBe("");
    expect(RegExp.$7).toBe("");
    expect(RegExp.$8).toBe("");
    expect(RegExp.$9).toBe("");
});

test("test should all behave as if exec were called.", () => {
    var re = /((\d+)\.(\d+))/;
    var s = "ghi789.012jkl";
    re.test(s);

    expect(RegExp.input).toBe(s);
    expect(RegExp["$_"]).toBe(s);
    expect(RegExp.lastMatch).toBe("789.012");
    expect(RegExp["$&"]).toBe("789.012");
    expect(RegExp.lastParen).toBe("012");
    expect(RegExp["$+"]).toBe("012");
    expect(RegExp.leftContext).toBe("ghi");
    expect(RegExp["$`"]).toBe("ghi");
    expect(RegExp.rightContext).toBe("jkl");
    expect(RegExp["$'"]).toBe("jkl");

    expect(RegExp.$1).toBe("789.012");
    expect(RegExp.$2).toBe("789");
    expect(RegExp.$3).toBe("012");
    expect(RegExp.$4).toBe("");
    expect(RegExp.$5).toBe("");
    expect(RegExp.$6).toBe("");
    expect(RegExp.$7).toBe("");
    expect(RegExp.$8).toBe("");
    expect(RegExp.$9).toBe("");
});

test("replace must interleave matching and replacing when a global regexp is matched and replaced with the result of a function", () => {
    var re = /(.)/g;
    function f() {
        return RegExp.$1;
    }
    expect("abcd".replace(re, f)).toBe("dddd");
});

test("the last parenthesis didn't match.", () => {
    expect(/foo(?:a(x))?/.exec("foobx")).toEqual(["foo", undefined]);
    expect(RegExp.lastParen).toBe("");
});

test("$1 to $9", () => {
    for (var i = 1; i <= 9; i++) {
        var haystack = "foo";
        var re_text = "^foo";
        for (var j = 0; j < i - 1; j++) {
            haystack += "x";
            re_text += "(x)";
        }
        re_text += "(?:a(x))?";
        haystack += "bx";
        var re = new RegExp(re_text);
        expect(re.test(haystack)).toBeTrue();
        for (var j = 1; j < i - 1; j++) {
            expect(RegExp["$" + j]).toBe("x");
        }
        expect(RegExp["$" + i]).toBe("");
    }
});

test("save the correct string as the last subject", () => {
    var foo =
        "lsdfj sldkfj sdklfj læsdfjl sdkfjlsdk fjsdl fjsdljskdj flsj flsdkj flskd regexp: /foobar/\nldkfj sdlkfj sdkl";
    expect(/^([a-z]+): (.*)/.test(foo.substring(foo.indexOf("regexp:")))).toBeTrue();
    expect(RegExp.$1).toBe("regexp");
});

test("calling with no argument is the same as calling with undefined.", () => {
    expect(/^undefined$/.test()).toBeTrue();
    expect(/^undefined$/.exec()).toEqual(["undefined"]);
});
