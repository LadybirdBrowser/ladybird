// JIT code reads characters for String.prototype.charCodeAt and charAt
// calls inline. These tests run every function many times, so that with a
// low JIT threshold they are compiled.

const iterations = 100;

function codes(string) {
    const result = [];
    for (let i = 0; i < string.length; ++i) result.push(string.charCodeAt(i));
    return result;
}

function characters(string) {
    const result = [];
    for (let i = 0; i < string.length; ++i) result.push(string.charAt(i));
    return result;
}

function codeAt(string, index) {
    return string.charCodeAt(index);
}

function characterAt(string, index) {
    return string.charAt(index);
}

describe("charCodeAt and charAt in JIT code", () => {
    test("short, long and wide strings", () => {
        const long = "the quick brown fox jumps over the lazy dog";
        const wide = "aé中😀z";
        for (let i = 0; i < iterations; ++i) {
            expect(codes("abc")).toEqual([97, 98, 99]);
            expect(characters("abc")).toEqual(["a", "b", "c"]);
            expect(String.fromCharCode(...codes(long))).toBe(long);
            expect(characters(long).join("")).toBe(long);
            expect(codes(wide)).toEqual([0x61, 0xe9, 0x4e2d, 0xd83d, 0xde00, 0x7a]);
            expect(characters(wide)).toEqual(["a", "é", "中", "\ud83d", "\ude00", "z"]);
            const rope = "prefix-" + i;
            expect(characters(rope).join("")).toBe(rope);
        }
    });

    test("indices outside the string and of other types", () => {
        for (let i = 0; i < iterations; ++i) {
            expect(codeAt("abc", 3)).toBeNaN();
            expect(codeAt("abc", -1)).toBeNaN();
            expect(codeAt("abc", 1.5)).toBe(98);
            expect(codeAt("abc", "2")).toBe(99);
            expect(codeAt("abc", undefined)).toBe(97);
            expect(characterAt("abc", 3)).toBe("");
            expect(characterAt("abc", -1)).toBe("");
            expect(characterAt("abc", 1.5)).toBe("b");
            expect(characterAt("abc", {})).toBe("a");
        }
    });

    test("other this values and replaced builtins", () => {
        for (let i = 0; i < iterations; ++i) {
            expect(codeAt(new String("xyz"), 1)).toBe(121);
            expect(characterAt(new String("xyz"), 1)).toBe("y");
            expect(codeAt({ charCodeAt: index => index * 2 }, 21)).toBe(42);
            expect(characterAt({ charAt: () => "mine" }, 0)).toBe("mine");
        }
        expect(() => codeAt(undefined, 0)).toThrow(TypeError);
        expect(() => String.prototype.charCodeAt.call(null, 0)).toThrow(TypeError);

        const original = String.prototype.charCodeAt;
        String.prototype.charCodeAt = function (index) {
            return `replaced ${index}`;
        };
        try {
            expect(codeAt("abc", 1)).toBe("replaced 1");
        } finally {
            String.prototype.charCodeAt = original;
        }
        expect(codeAt("abc", 1)).toBe(98);
    });
});
