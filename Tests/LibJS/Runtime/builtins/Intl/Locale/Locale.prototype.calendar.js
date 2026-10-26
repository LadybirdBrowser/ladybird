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

    test("option values are canonicalized without regard to case", () => {
        const values = [
            ["GREGORY", "gregory"],
            ["UNKNOWN", "unknown"],
            ["ISLAMICC", "islamic-civil"],
            ["ethiopic-AMETE-alem", "ethioaa"],
        ];

        for (const tag of ["en", "en-u-ca-abc"]) {
            for (const [value, expected] of values) {
                const locale = new Intl.Locale(tag, { calendar: value });
                expect(locale.calendar).toBe(expected);
                expect(locale.toString()).toBe(`en-u-ca-${expected}`);
            }
        }
    });
});
