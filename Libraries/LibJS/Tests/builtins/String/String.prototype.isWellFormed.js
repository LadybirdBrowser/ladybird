describe("errors", () => {
    test("called with value that cannot be converted to a string", () => {
        expect(() => {
            String.prototype.isWellFormed.call(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to string");
    });
});

describe("basic functionality", () => {
    test("ascii strings", () => {
        expect("".isWellFormed()).toBeTrue();
        expect("foo".isWellFormed()).toBeTrue();
        expect("abcdefghi".isWellFormed()).toBeTrue();
    });

    test("valid UTF-16 strings", () => {
        expect("😀".isWellFormed()).toBeTrue();
        expect("\ud83d\ude00".isWellFormed()).toBeTrue();
    });

    test("invalid UTF-16 strings", () => {
        expect("😀".slice(0, 1).isWellFormed()).toBeFalse();
        expect("😀".slice(1, 2).isWellFormed()).toBeFalse();
        expect("\ud83d".isWellFormed()).toBeFalse();
        expect("\ude00".isWellFormed()).toBeFalse();
        expect("a\ud83d".isWellFormed()).toBeFalse();
        expect("a\ude00".isWellFormed()).toBeFalse();
        expect("\ud83da".isWellFormed()).toBeFalse();
        expect("\ude00a".isWellFormed()).toBeFalse();
        expect("a\ud83da".isWellFormed()).toBeFalse();
        expect("a\ude00a".isWellFormed()).toBeFalse();
    });

    test("object converted to a string", () => {
        let toStringCalled = false;

        const obj = {
            toString: function () {
                toStringCalled = true;
                return "toString";
            },
        };

        expect(String.prototype.isWellFormed.call(obj)).toBeTrue();
        expect(toStringCalled).toBeTrue();
    });
});
