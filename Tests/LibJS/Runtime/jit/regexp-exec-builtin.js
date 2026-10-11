// JIT code calls RegExp.prototype.exec through its runtime helper where
// RegExpBuiltinExec runs no code, and calls the function for anything else.

function compileCalls(f, ...args) {
    jit.prepare(f);
    for (let i = 0; i < 10; ++i) f(...args);
    expect(jit.compile(f)).toBe(jit.enabled);
    return f;
}

describe("RegExp.prototype.exec in JIT code", () => {
    test("matches, captures and misses", () => {
        const exec = compileCalls((re, s) => re.exec(s), /(a)(b)?/, "xa");
        let match = exec(/(a)(b)?/, "xab");
        expect(match).toEqual(["ab", "a", "b"]);
        expect(match.index).toBe(1);
        expect(match.input).toBe("xab");
        match = exec(/(a)(b)?/, "xa");
        expect(match[2]).toBeUndefined();
        expect(exec(/a/, "zzz")).toBeNull();
        expect(exec(/(?<letter>[a-z])/, "1q").groups.letter).toBe("q");
        expect(RegExp.$1).toBe("q");
        expect(jit.exitCount(exec)).toBe(0);
    });

    test("global and sticky regular expressions update lastIndex", () => {
        const exec = compileCalls((re, s) => re.exec(s), /a/g, "aa");
        const global = /a/g;
        expect(exec(global, "aba").index).toBe(0);
        expect(global.lastIndex).toBe(1);
        expect(exec(global, "aba").index).toBe(2);
        expect(global.lastIndex).toBe(3);
        expect(exec(global, "aba")).toBeNull();
        expect(global.lastIndex).toBe(0);
        const sticky = /a/y;
        sticky.lastIndex = 1;
        expect(exec(sticky, "aba")).toBeNull();
        expect(sticky.lastIndex).toBe(0);
        global.lastIndex = 100;
        expect(exec(global, "aba")).toBeNull();
        expect(global.lastIndex).toBe(0);
        const nonGlobal = /a/;
        nonGlobal.lastIndex = 2;
        expect(exec(nonGlobal, "aba").index).toBe(0);
        expect(nonGlobal.lastIndex).toBe(2);
    });

    test("observable lastIndex values go through the function", () => {
        const exec = compileCalls((re, s) => re.exec(s), /a/g, "aa");
        const regexp = /a/g;
        let conversions = 0;
        regexp.lastIndex = {
            valueOf() {
                ++conversions;
                return 1;
            },
        };
        expect(exec(regexp, "aa").index).toBe(1);
        expect(conversions).toBe(1);
        expect(regexp.lastIndex).toBe(2);
        const frozen = /a/g;
        Object.freeze(frozen);
        expect(() => exec(frozen, "a")).toThrow(TypeError);
        const readOnly = /a/g;
        Object.defineProperty(readOnly, "lastIndex", { writable: false });
        expect(() => exec(readOnly, "a")).toThrow(TypeError);
        const extended = /a/g;
        extended.extra = 1;
        expect(exec(extended, "aa").index).toBe(0);
        expect(extended.lastIndex).toBe(1);
        const frozenNonGlobal = Object.freeze(/a/);
        expect(exec(frozenNonGlobal, "a").index).toBe(0);
    });

    test("other receivers and arguments go through the function", () => {
        const exec = compileCalls((re, s) => re.exec(s), /a/, "a");
        expect(exec(/1/, 1).index).toBe(0);
        expect(() => exec({ exec: RegExp.prototype.exec }, "a")).toThrow(TypeError);
        expect(exec({ exec: () => "own" }, "a")).toBe("own");
        const execCall = compileCalls((re, s) => RegExp.prototype.exec.call(re, s), /a/, "a");
        expect(() => execCall("not a regexp", "a")).toThrow(TypeError);
    });
});
