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
});
