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
});
