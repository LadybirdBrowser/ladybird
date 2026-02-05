describe("errors", () => {
    test("called on non-Locale object", () => {
        expect(() => {
            Intl.Locale.prototype.calendar;
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.Locale");
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        expect(new Intl.Locale("en").calendar).toBeUndefined();
        expect(new Intl.Locale("en-u-ca-abc").calendar).toBe("abc");
        expect(new Intl.Locale("en", { calendar: "abc" }).calendar).toBe("abc");
        expect(new Intl.Locale("en-u-ca-abc", { calendar: "def" }).calendar).toBe("def");

        expect(new Intl.Locale("en", { calendar: "islamicc" }).calendar).toBe("islamic-civil");
        expect(new Intl.Locale("en-u-ca-islamicc").calendar).toBe("islamic-civil");

        expect(new Intl.Locale("en", { calendar: "ethiopic-amete-alem" }).calendar).toBe("ethioaa");
        expect(new Intl.Locale("en-u-ca-ethiopic-amete-alem").calendar).toBe("ethioaa");
    });
});
