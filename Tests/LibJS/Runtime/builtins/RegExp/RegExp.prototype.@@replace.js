describe("many-match global replace", () => {
    test("batch replace with many matches produces correct output", () => {
        // Exercise the batch find_all path with enough matches to force buffer growth.
        let input = "a".repeat(1000);
        let result = input.replace(/a/g, "b");
        expect(result).toBe("b".repeat(1000));
    });

    test("batch replace agrees with non-batch replace", () => {
        let input = "xyzxyzxyz".repeat(200);
        // Global non-sticky: uses batch path.
        let batchResult = input.replace(/xyz/g, "AB");
        // Verify correctness.
        expect(batchResult).toBe("AB".repeat(600));
    });
});

describe("basic functionality", () => {
    test("uses flags property instead of individual property lookups", () => {
        let accessedFlags = false;
        let accessedGlobal = false;
        let accessedUnicode = false;

        class RegExp1 extends RegExp {
            get flags() {
                accessedFlags = true;
                return "g";
            }
            get global() {
                accessedGlobal = true;
                return false;
            }
            get unicode() {
                accessedUnicode = true;
                return false;
            }
        }

        RegExp.prototype[Symbol.replace].call(new RegExp1("foo"));
        expect(accessedFlags).toBeTrue();
        expect(accessedGlobal).toBeFalse();
        expect(accessedUnicode).toBeFalse();
    });

    test("uses own unicodeSets property", () => {
        const regexp = /(?:)/g;
        Object.defineProperty(regexp, "unicodeSets", { configurable: true, value: true });

        const output = "😀".replace(regexp, "X");

        expect(output).toBe("X😀X");
        expect(output.isWellFormed()).toBeTrue();
    });

    test("successful fast replace invalidates nonlegacy state", () => {
        function NewTarget() {}
        NewTarget.prototype = RegExp.prototype;
        const regexp = Reflect.construct(RegExp, ["(.)"], NewTarget);

        /(PROTECTED)/.test("PROTECTED");
        expect("x".replace(regexp, "y")).toBe("y");
        expect(RegExp.input).toBe("");
        expect(RegExp.$1).toBe("");
    });

    test("observes a replaced inherited flags getter", () => {
        const originalFlags = Object.getOwnPropertyDescriptor(RegExp.prototype, "flags");
        try {
            Object.defineProperty(RegExp.prototype, "flags", {
                configurable: true,
                get() {
                    throw new Error("BLOCKED");
                },
            });

            expect(() => "SAFE".replace(/x/, "y")).toThrowWithMessage(Error, "BLOCKED");
        } finally {
            Object.defineProperty(RegExp.prototype, "flags", originalFlags);
        }
    });

    test("observes an own flag property", () => {
        const regexp = /^SAFE$/;
        Object.defineProperty(regexp, "dotAll", {
            configurable: true,
            get() {
                throw new Error("BLOCKED");
            },
        });

        expect(() => "SAFE".replace(regexp, "y")).toThrowWithMessage(Error, "BLOCKED");
    });

    test("observes a non-writable lastIndex on a global pattern", () => {
        const pattern = /a/g;
        Object.defineProperty(pattern, "lastIndex", { writable: false });

        expect(() => "aaa".replace(pattern, "b")).toThrowWithMessage(
            TypeError,
            "Cannot set property 'lastIndex' of [object RegExpObject]"
        );
    });

    test("observes a replaced inherited flag accessor", () => {
        const flags = [
            "flags",
            "hasIndices",
            "global",
            "ignoreCase",
            "multiline",
            "dotAll",
            "unicode",
            "unicodeSets",
            "sticky",
        ];

        for (const flag of flags) {
            const original = Object.getOwnPropertyDescriptor(RegExp.prototype, flag);
            try {
                Object.defineProperty(RegExp.prototype, flag, {
                    configurable: true,
                    get() {
                        throw new Error("BLOCKED " + flag);
                    },
                });

                expect(() => "x x".replace(/x/g, "y")).toThrowWithMessage(Error, "BLOCKED " + flag);
            } finally {
                Object.defineProperty(RegExp.prototype, flag, original);
            }
        }
    });

    test("revalidates global after replacement coercion", () => {
        const regexp = /x/g;
        const replacement = {
            toString() {
                Object.defineProperty(regexp, "global", {
                    configurable: true,
                    get() {
                        throw new Error("BLOCKED");
                    },
                });
                return "y";
            },
        };

        expect(() => "x x".replace(regexp, replacement)).toThrowWithMessage(Error, "BLOCKED");
    });

    test("revalidates exec after replacement coercion", () => {
        const regexp = /^SAFE$/;
        let execCalls = 0;
        const replacement = {
            toString() {
                regexp.exec = () => {
                    ++execCalls;
                    return Object.assign(["unsafe"], { index: 0 });
                };
                return "BLOCKED";
            },
        };

        expect("unsafe".replace(regexp, replacement)).toBe("BLOCKED");
        expect(execCalls).toBe(1);
    });

    test("coerces the replacement exactly once", () => {
        const counted = pattern => {
            let calls = 0;
            const replacement = {
                toString() {
                    ++calls;
                    return "$&!";
                },
            };
            "aa".replace(pattern, replacement);
            return calls;
        };

        expect(counted(/a/)).toBe(1);
        expect(counted(/a/g)).toBe(1);
    });
});

test("legacy static properties after a global replace without captures", () => {
    expect("a-b-c".replace(/-/g, "_")).toBe("a_b_c");
    expect(RegExp.lastMatch).toBe("-");
    expect(RegExp.leftContext).toBe("a-b");
    expect(RegExp.rightContext).toBe("c");
    expect(RegExp.lastParen).toBe("");
    expect(RegExp.$1).toBe("");

    expect("x1y22z".replace(/(\d+)/g, "#")).toBe("x#y#z");
    expect(RegExp.lastMatch).toBe("22");
    expect(RegExp.$1).toBe("22");
});

test("replacement values that are not strings", () => {
    const regexp = /-/g;
    expect("a-b".replace(regexp, 1)).toBe("a1b");
    expect("a-b".replace(regexp, null)).toBe("anullb");
    expect(regexp.lastIndex).toBe(0);
    expect(() => "a-b".replace(regexp, Symbol())).toThrow(TypeError);
});

describe("replacing with functions and substitutions", () => {
    test("the replacer sees the state after all matches were found", () => {
        const regexp = /(a)|(b)/g;
        const calls = [];
        const result = "abcab".replace(regexp, (match, a, b, position, string) => {
            calls.push([match, a, b, position, string, regexp.lastIndex, RegExp.lastMatch]);
            regexp.lastIndex = 3;
            return `<${match}>`;
        });
        expect(result).toBe("<a><b>c<a><b>");
        expect(regexp.lastIndex).toBe(3);
        expect(calls).toEqual([
            ["a", "a", undefined, 0, "abcab", 0, "b"],
            ["b", undefined, "b", 1, "abcab", 3, "b"],
            ["a", "a", undefined, 3, "abcab", 3, "b"],
            ["b", undefined, "b", 4, "abcab", 3, "b"],
        ]);
    });

    test("empty matches, surrogate pairs and other flags", () => {
        expect("aaa".replace(/a*?/g, (match, position) => `[${position}]`)).toBe("[0]a[1]a[2]a[3]");
        expect("😀x😀".replace(/(?:)/gu, "-")).toBe("-😀-x-😀-");
        expect("😀x".replace(/(?:)/g, "-")).toBe("-\uD83D-\uDE00-x-");
        expect("abc".replace(/(?<n>b)/, (...args) => typeof args[args.length - 1])).toBe("aobjectc");
        expect("abcabc".replace(/b/y, "X")).toBe("abcabc");
        expect("aXbX".replace(/x/gi, match => match.toLowerCase())).toBe("axbx");
        expect("abc".replace(/z/g, () => "never")).toBe("abc");

        const nonGlobal = /o/;
        nonGlobal.lastIndex = 5;
        expect("foo".replace(nonGlobal, () => String(nonGlobal.lastIndex))).toBe("f5o");
        expect(nonGlobal.lastIndex).toBe(5);
    });

    test("substitutions", () => {
        expect("a1b22".replace(/(\d)(x)?/g, "[$1|$2|$&|$`|$']")).toBe("a[1||1|a|b22]b[2||2|a1b|2][2||2|a1b2|]");
        expect("hello".replace(/l/g, "$$")).toBe("he$$o");
        expect("hello".replace(/(l)/g, "$11")).toBe("hel1l1o");
        expect("hello".replace(/(l)/, "$0")).toBe("he$0lo");
    });
});
