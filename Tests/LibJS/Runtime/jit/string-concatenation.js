// JIT code concatenates two strings into a rope string inline. These tests run
// every function many times, so that with a low JIT threshold they are
// compiled after seeing strings.

const iterations = 100;

describe("string concatenation in JIT code", () => {
    test("ropes, empty strings and short strings", () => {
        function concatenate(a, b) {
            return a + b;
        }
        for (let i = 0; i < iterations; ++i) {
            const long = concatenate("a fairly long string ", "and another one " + i);
            expect(long).toBe("a fairly long string and another one " + i);
            expect(long.length).toBe(37 + String(i).length);
            expect(concatenate("", "right")).toBe("right");
            expect(concatenate("left", "")).toBe("left");
            expect(concatenate("", "")).toBe("");
            expect(concatenate("ab", "cd")).toBe("abcd");
            expect(concatenate("été ", "et hiver, saisons longues")).toBe("été et hiver, saisons longues");
            expect(concatenate(1, 2)).toBe(3);
            expect(concatenate(1.5, "x")).toBe("1.5x");
            expect(concatenate("x", { toString: () => "y" })).toBe("xy");
        }
    });

    test("ropes of ropes, with collections in between", () => {
        function append(buffer, piece) {
            buffer += piece;
            return buffer;
        }
        for (let i = 0; i < iterations; ++i) {
            let buffer = "";
            let expected = "";
            for (let j = 0; j < 20; ++j) {
                const piece = '<li data-id="' + j + '">item ' + i + "</li>";
                buffer = append(buffer, piece);
                expected = expected.concat(piece);
                if (j % 7 === 0) gc();
            }
            expect(buffer).toBe(expected);
            expect(buffer.length).toBe(expected.length);
            expect(buffer.charCodeAt(buffer.length - 1)).toBe(62);
        }
    });
});
