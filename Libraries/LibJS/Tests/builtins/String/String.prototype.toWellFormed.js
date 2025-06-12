describe("errors", () => {
    test("called with value that cannot be converted to a string", () => {
        expect(() => {
            String.prototype.toWellFormed.call(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to string");
    });
});

describe("basic functionality", () => {
    test("ascii strings", () => {
        expect("".toWellFormed()).toBe("");
        expect("foo".toWellFormed()).toBe("foo");
        expect("abcdefghi".toWellFormed()).toBe("abcdefghi");
    });

    test("valid UTF-16 strings", () => {
        expect("😀".toWellFormed()).toBe("😀");
        expect("\ud83d\ude00".toWellFormed()).toBe("\ud83d\ude00");
    });

    test("invalid UTF-16 strings", () => {
        expect("😀".slice(0, 1).toWellFormed()).toBe("\ufffd");
        expect("😀".slice(1, 2).toWellFormed()).toBe("\ufffd");
        expect("\ud83d".toWellFormed()).toBe("\ufffd");
        expect("\ude00".toWellFormed()).toBe("\ufffd");
        expect("a\ud83d".toWellFormed()).toBe("a\ufffd");
        expect("a\ude00".toWellFormed()).toBe("a\ufffd");
        expect("\ud83da".toWellFormed()).toBe("\ufffda");
        expect("\ude00a".toWellFormed()).toBe("\ufffda");
        expect("a\ud83da".toWellFormed()).toBe("a\ufffda");
        expect("a\ude00a".toWellFormed()).toBe("a\ufffda");
    });

    test("object converted to a string", () => {
        let toStringCalled = false;

        const obj = {
            toString: function () {
                toStringCalled = true;
                return "toString";
            },
        };

        expect(String.prototype.toWellFormed.call(obj)).toBe("toString");
        expect(toStringCalled).toBeTrue();
    });
});
