// JIT code calls String.prototype builtins like indexOf, slice and trim
// through their runtime helpers without a native frame for strings and int32
// positions, and calls the functions for anything else.

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

    test("lastIndexOf, includes, startsWith and endsWith of strings", () => {
        const lastIndexOf = compileCalls((s, t) => s.lastIndexOf(t), "abcabc", "b");
        const lastIndexOfFrom = compileCalls((s, t, p) => s.lastIndexOf(t, p), "abcabc", "b", 3);
        const includes = compileCalls((s, t) => s.includes(t), "abc", "b");
        const includesFrom = compileCalls((s, t, p) => s.includes(t, p), "abc", "b", 1);
        const startsWith = compileCalls((s, t) => s.startsWith(t), "abc", "a");
        const startsWithAt = compileCalls((s, t, p) => s.startsWith(t, p), "abc", "b", 1);
        const endsWith = compileCalls((s, t) => s.endsWith(t), "abc", "c");
        const endsWithAt = compileCalls((s, t, p) => s.endsWith(t, p), "abc", "b", 2);
        expect(lastIndexOf("abcabc", "b")).toBe(4);
        expect(lastIndexOf("abcabc", "z")).toBe(-1);
        expect(lastIndexOf("abc", "")).toBe(3);
        expect(lastIndexOf("ab", "abc")).toBe(-1);
        expect(lastIndexOfFrom("abcabc", "b", 3)).toBe(1);
        expect(lastIndexOfFrom("abcabc", "b", -1)).toBe(-1);
        expect(lastIndexOfFrom("abcabc", "a", -1)).toBe(0);
        expect(lastIndexOfFrom("abcabc", "b", 2147483647)).toBe(4);
        expect(includes("hello", "ell")).toBeTrue();
        expect(includes("hello", "")).toBeTrue();
        expect(includes("hello", "x")).toBeFalse();
        expect(includesFrom("hello", "h", 1)).toBeFalse();
        expect(includesFrom("hello", "h", -3)).toBeTrue();
        expect(startsWith("hello", "he")).toBeTrue();
        expect(startsWith("hello", "hello!")).toBeFalse();
        expect(startsWithAt("hello", "lo", 3)).toBeTrue();
        expect(startsWithAt("hello", "", 100)).toBeTrue();
        expect(endsWith("hello", "lo")).toBeTrue();
        expect(endsWith("hello", "he")).toBeFalse();
        expect(endsWithAt("hello", "he", 2)).toBeTrue();
        expect(endsWithAt("hello", "lo", 2147483647)).toBeTrue();
        expect(endsWithAt("hello", "h", -5)).toBeFalse();
        for (const f of [
            lastIndexOf,
            lastIndexOfFrom,
            includes,
            includesFrom,
            startsWith,
            startsWithAt,
            endsWith,
            endsWithAt,
        ])
            expect(jit.exitCount(f)).toBe(0);
    });

    test("lastIndexOf, includes, startsWith and endsWith of other values", () => {
        const lastIndexOfFrom = compileCalls((s, t, p) => s.lastIndexOf(t, p), "abcabc", "b", 3);
        const includes = compileCalls((s, t) => s.includes(t), "abc", "b");
        const startsWithAt = compileCalls((s, t, p) => s.startsWith(t, p), "abc", "b", 1);
        const endsWith = compileCalls((s, t) => s.endsWith(t), "abc", "c");
        expect(lastIndexOfFrom("abcabc", "b", NaN)).toBe(4);
        expect(lastIndexOfFrom("abcabc", "b", undefined)).toBe(4);
        expect(lastIndexOfFrom("abcabc", "b", 3.9)).toBe(1);
        expect(includes("a1", 1)).toBeTrue();
        expect(() => includes("abc", /b/)).toThrow(TypeError);
        expect(() => endsWith("abc", /c/)).toThrow(TypeError);
        expect(startsWithAt("abc", "b", "1")).toBeTrue();
        expect(startsWithAt("abc", "a", -Infinity)).toBeTrue();
        expect(endsWith(new String("abc"), "c")).toBeTrue();
    });

    test("slice and substring of strings", () => {
        const slice = compileCalls((s, a, b) => s.slice(a, b), "abcdef", 1, 3);
        const sliceFrom = compileCalls((s, a) => s.slice(a), "abcdef", 1);
        const sliceAll = compileCalls(s => s.slice(), "abcdef");
        const substring = compileCalls((s, a, b) => s.substring(a, b), "abcdef", 1, 3);
        const substringFrom = compileCalls((s, a) => s.substring(a), "abcdef", 1);
        expect(slice("abcdef", 1, 3)).toBe("bc");
        expect(slice("abcdef", -3, -1)).toBe("de");
        expect(slice("abcdef", 4, 2)).toBe("");
        expect(slice("abcdef", -100, 100)).toBe("abcdef");
        expect(slice("abcdef", 2, 2147483647)).toBe("cdef");
        expect(slice("aé中😀z", 3, 5)).toBe("😀");
        expect(sliceFrom("abcdef", -2)).toBe("ef");
        expect(sliceAll("abc")).toBe("abc");
        expect(substring("abcdef", 1, 3)).toBe("bc");
        expect(substring("abcdef", 3, 1)).toBe("bc");
        expect(substring("abcdef", -3, 2)).toBe("ab");
        expect(substring("abcdef", 2, 2147483647)).toBe("cdef");
        expect(substringFrom("abcdef", 4)).toBe("ef");
        expect(substringFrom("rope-" + slice.length, 2)).toBe("pe-3");
        for (const f of [slice, sliceFrom, sliceAll, substring, substringFrom]) expect(jit.exitCount(f)).toBe(0);
    });

    test("slice and substring of other values", () => {
        const slice = compileCalls((s, a, b) => s.slice(a, b), "abcdef", 1, 3);
        const substring = compileCalls((s, a, b) => s.substring(a, b), "abcdef", 1, 3);
        expect(slice("abcdef", 1.5, 3.5)).toBe("bc");
        expect(slice("abcdef", 1, undefined)).toBe("bcdef");
        expect(slice("abcdef", -Infinity, Infinity)).toBe("abcdef");
        expect(slice([1, 2, 3], 1, 2)).toEqual([2]);
        expect(substring("abcdef", "1", NaN)).toBe("a");
        expect(substring(new String("12345"), 1, 3)).toBe("23");
    });

    test("trim, trimStart and trimEnd of strings", () => {
        const trim = compileCalls(s => s.trim(), " a ");
        const trimStart = compileCalls(s => s.trimStart(), " a ");
        const trimEnd = compileCalls(s => s.trimEnd(), " a ");
        expect(trim("  a b  ")).toBe("a b");
        expect(trim("ab")).toBe("ab");
        expect(trim("   ")).toBe("");
        expect(trim("\t\n\u00a0\ufeffx\u2028")).toBe("x");
        expect(trimStart("  a  ")).toBe("a  ");
        expect(trimEnd("  a  ")).toBe("  a");
        expect(trim("  rope-" + trim.length + "  ")).toBe("rope-1");
        for (const f of [trim, trimStart, trimEnd]) expect(jit.exitCount(f)).toBe(0);
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
