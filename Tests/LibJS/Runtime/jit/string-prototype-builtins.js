// JIT code calls String.prototype.indexOf, toLowerCase and toUpperCase
// through the runtime without a native frame for strings, and calls the
// functions for anything else.

function compileCalls(f, ...args) {
    jit.prepare(f);
    for (let i = 0; i < 10; ++i) f(...args);
    expect(jit.compile(f)).toBe(jit.enabled);
    return f;
}

describe("String.prototype builtins in JIT code", () => {
    test("indexOf of strings", () => {
        const indexOf = compileCalls((s, t) => s.indexOf(t), "abc", "b");
        const indexOfFrom = compileCalls((s, t, p) => s.indexOf(t, p), "abcabc", "b", 2);
        expect(indexOf("hello", "l")).toBe(2);
        expect(indexOf("hello", "z")).toBe(-1);
        expect(indexOf("hello", "")).toBe(0);
        expect(indexOf("", "a")).toBe(-1);
        expect(indexOf("aé中😀z", "😀")).toBe(3);
        expect(indexOf("prefix-" + indexOf.length, "-1")).toBe(-1);
        expect(indexOf("prefix-" + 12, "-1")).toBe(6);
        expect(indexOfFrom("abcabc", "b", 2)).toBe(4);
        expect(indexOfFrom("abcabc", "b", -5)).toBe(1);
        expect(indexOfFrom("abcabc", "b", 100)).toBe(-1);
        expect(indexOfFrom("abc", "", 100)).toBe(3);
        expect(jit.exitCount(indexOf)).toBe(0);
        expect(jit.exitCount(indexOfFrom)).toBe(0);
    });

    test("indexOf of other values", () => {
        const indexOf = compileCalls((s, t) => s.indexOf(t), "abc", "b");
        const indexOfFrom = compileCalls((s, t, p) => s.indexOf(t, p), "abcabc", "b", 2);
        expect(indexOf("a1b", 1)).toBe(1);
        expect(indexOf("an undefined", undefined)).toBe(3);
        expect(indexOf(new String("abc"), "c")).toBe(2);
        expect(() => indexOf(null, "a")).toThrow(TypeError);
        expect(indexOfFrom("abcabc", "b", 1.5)).toBe(1);
        expect(indexOfFrom("abcabc", "b", "3")).toBe(4);
        expect(indexOfFrom("abcabc", "b", undefined)).toBe(1);
        expect(indexOfFrom("abcabc", "b", Infinity)).toBe(-1);
        expect(indexOfFrom("abcabc", "b", -Infinity)).toBe(1);
        let conversions = 0;
        const counted = {
            toString() {
                ++conversions;
                return "c";
            },
        };
        expect(indexOf("abc", counted)).toBe(2);
        expect(conversions).toBe(1);
    });

    test("toLowerCase and toUpperCase of strings", () => {
        const lower = compileCalls(s => s.toLowerCase(), "ABC");
        const upper = compileCalls(s => s.toUpperCase(), "abc");
        expect(lower("HeLLo")).toBe("hello");
        expect(upper("HeLLo")).toBe("HELLO");
        expect(lower("")).toBe("");
        expect(upper("ß")).toBe("SS");
        expect(lower("İ")).toBe("i̇");
        expect(lower("ÀÉÎ")).toBe("àéî");
        expect(upper("rope-" + lower.length)).toBe("ROPE-1");
        expect(jit.exitCount(lower)).toBe(0);
        expect(jit.exitCount(upper)).toBe(0);
    });

    test("toLowerCase and toUpperCase of other values", () => {
        const lower = compileCalls(s => s.toLowerCase(), "ABC");
        const upper = compileCalls(s => s.toUpperCase(), "abc");
        expect(lower(new String("ABC"))).toBe("abc");
        expect(() => upper(undefined)).toThrow(TypeError);
        expect(lower({ toLowerCase: () => "own" })).toBe("own");
    });

    test("replaced functions are called", () => {
        const indexOf = compileCalls((s, t) => s.indexOf(t), "abc", "b");
        const original = String.prototype.indexOf;
        String.prototype.indexOf = () => "replaced";
        try {
            expect(indexOf("abc", "b")).toBe("replaced");
        } finally {
            String.prototype.indexOf = original;
        }
        expect(indexOf("abc", "b")).toBe(1);
    });
});
