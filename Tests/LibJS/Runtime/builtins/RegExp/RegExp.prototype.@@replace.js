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
