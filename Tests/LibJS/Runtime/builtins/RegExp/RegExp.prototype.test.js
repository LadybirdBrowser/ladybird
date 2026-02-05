test("basic functionality", () => {
    expect(RegExp.prototype.test).toHaveLength(1);
});

test("simple test", () => {
    let re = /test/;
    expect(re.test("test")).toBe(true);
    expect(re.test("test")).toBe(true);
});

test("simple global test", () => {
    let re = /test/g;
    expect(re.test("testtest")).toBe(true);
    expect(re.test("testtest")).toBe(true);
    expect(re.test("testtest")).toBe(false);
    expect(re.test("testtest")).toBe(true);
    expect(re.test("testtest")).toBe(true);
});

test("global test with offset lastIndex", () => {
    let re = /test/g;
    re.lastIndex = 2;
    expect(re.test("testtest")).toBe(true);
    expect(re.test("testtest")).toBe(false);
    expect(re.test("testtest")).toBe(true);
    expect(re.test("testtest")).toBe(true);
    expect(re.test("testtest")).toBe(false);
});

test("sticky test with offset lastIndex", () => {
    let re = /test/y;
    re.lastIndex = 2;
    expect(re.test("aatest")).toBe(true);
    expect(re.test("aatest")).toBe(false);
    expect(re.test("aatest")).toBe(false);
});

test("flag and options", () => {
    expect(/foo/gi.flags).toBe("gi");
    expect(/foo/mu.flags).toBe("mu");
    expect(/foo/gimsuy.flags).toBe("gimsuy");

    let re = /foo/gim;
    expect(re.dotAll).toBe(false);
    expect(re.global).toBe(true);
    expect(re.ignoreCase).toBe(true);
    expect(re.multiline).toBe(true);
    expect(re.sticky).toBe(false);
    expect(re.unicode).toBe(false);

    expect(() => {
        Function("/foo/gg");
    }).toThrowWithMessage(SyntaxError, "Repeated RegExp flag 'g'");

    expect(() => {
        Function("/foo/x");
    }).toThrowWithMessage(SyntaxError, "Invalid RegExp flag 'x'");
});

test("override exec with function", () => {
    let calls = 0;

    let re = /test/;
    let oldExec = re.exec.bind(re);
    re.exec = function (...args) {
        ++calls;
        return oldExec(...args);
    };

    expect(re.test("test")).toBe(true);
    expect(calls).toBe(1);
});

test("override exec with bad function", () => {
    let calls = 0;

    let re = /test/;
    re.exec = function (...args) {
        ++calls;
        return 4;
    };

    expect(() => {
        re.test("test");
    }).toThrow(TypeError);
    expect(calls).toBe(1);
});

test("override exec with non-function", () => {
    let re = /test/;
    re.exec = 3;
    expect(re.test("test")).toBe(true);
});

test("property escapes", () => {
    expect(/\p{ASCII}/.test("a")).toBeFalse();
    expect(/\p{ASCII}/.test("p{ASCII}")).toBeTrue();
    expect(/\p{ASCII}/u.test("a")).toBeTrue();
    expect(/\p{ASCII}/u.test("😀")).toBeFalse();
    expect(/\p{ASCII_Hex_Digit}/u.test("1")).toBeTrue();
    expect(/\p{ASCII_Hex_Digit}/u.test("a")).toBeTrue();
    expect(/\p{ASCII_Hex_Digit}/u.test("x")).toBeFalse();
    expect(/\p{Any}/u.test("\u0378")).toBeTrue();
    expect(/\p{Assigned}/u.test("\u0378")).toBeFalse();
});
