// JIT code reads the characters of strings by index inline. These tests run
// every function many times, so that with a low JIT threshold they are
// compiled.

const iterations = 100;

function at(string, index) {
    return string[index];
}

function characters(string) {
    const result = [];
    for (let i = 0; i < string.length; ++i) result.push(string[i]);
    return result;
}

describe("String characters in JIT code", () => {
    test("short and long strings", () => {
        const long = "the quick brown fox jumps over the lazy dog";
        for (let i = 0; i < iterations; ++i) {
            expect(characters("abc")).toEqual(["a", "b", "c"]);
            expect(characters(long).join("")).toBe(long);
            expect(at(long, i % long.length)).toBe(long.charAt(i % long.length));
        }
    });

    test("characters beyond ASCII", () => {
        const string = "aé中😀z";
        for (let i = 0; i < iterations; ++i) {
            expect(characters(string)).toEqual(["a", "é", "中", "\ud83d", "\ude00", "z"]);
        }
    });

    test("strings made at run time", () => {
        for (let i = 0; i < iterations; ++i) {
            const rope = "prefix-" + i + "-suffix";
            expect(at(rope, 7)).toBe(String(i)[0]);
            expect(characters(rope).join("")).toBe(rope);
            const slice = rope.slice(1, 5);
            expect(at(slice, 0)).toBe("r");
        }
    });

    test("indices outside the string", () => {
        for (let i = 0; i < iterations; ++i) {
            expect(at("abc", 3)).toBeUndefined();
            expect(at("abc", -1)).toBeUndefined();
            expect(at("abc", 1.5)).toBeUndefined();
            expect(at("abc", 1)).toBe("b");
            expect(at("abc", "1")).toBe("b");
            expect(at("abc", "length")).toBe(3);
        }
        String.prototype[3] = "from the prototype";
        try {
            expect(at("abc", 3)).toBe("from the prototype");
            expect(at("abc", 2)).toBe("c");
        } finally {
            delete String.prototype[3];
        }
        expect(at("abc", 3)).toBeUndefined();
    });

    test("other bases", () => {
        for (let i = 0; i < iterations; ++i) {
            expect(at(new String("xyz"), 1)).toBe("y");
            expect(at(["x", "y"], 1)).toBe("y");
            expect(at(5, 0)).toBeUndefined();
        }
        expect(() => at(undefined, 0)).toThrow(TypeError);
        expect(() => at(null, 0)).toThrow(TypeError);
    });
});
