describe("errors", () => {
    test("called on non-Locale object", () => {
        expect(() => {
            Intl.Locale.prototype.firstDayOfWeek;
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.Locale");
    });

    test("invalid options", () => {
        [123456789, "a", "longerthan8chars"].forEach(value => {
            expect(() => {
                new Intl.Locale("en", { firstDayOfWeek: value }).firstDayOfWeek;
            }).toThrowWithMessage(RangeError, `${value} is not a valid value for option firstDayOfWeek`);
        });
    });
});

describe("normal behavior", () => {
    test("standard options", () => {
        expect(new Intl.Locale("en").firstDayOfWeek).toBeUndefined();

        ["sun", "mon", "tue", "wed", "thu", "fri", "sat", "sun"].forEach((day, index) => {
            expect(new Intl.Locale(`en-u-fw-${day}`).firstDayOfWeek).toBe(day);

            expect(new Intl.Locale("en", { firstDayOfWeek: day }).firstDayOfWeek).toBe(day);
            expect(new Intl.Locale("en", { firstDayOfWeek: index }).firstDayOfWeek).toBe(day);

            expect(new Intl.Locale("en-u-fw-mon", { firstDayOfWeek: day }).firstDayOfWeek).toBe(day);
            expect(new Intl.Locale("en-u-fw-mon", { firstDayOfWeek: index }).firstDayOfWeek).toBe(day);
        });
    });

    test("non-standard options", () => {
        [100, Infinity, NaN, "hello", 152n].forEach(value => {
            expect(new Intl.Locale("en", { firstDayOfWeek: value }).firstDayOfWeek).toBe(
                value.toString().toLowerCase()
            );
        });
    });

    test("option values are canonicalized without regard to case", () => {
        for (const tag of ["en", "en-u-fw-tue"]) {
            for (const value of ["MON", "Unknown", NaN, Infinity]) {
                const expected = String(value).toLowerCase();
                const locale = new Intl.Locale(tag, { firstDayOfWeek: value });
                expect(locale.firstDayOfWeek).toBe(expected);
                expect(locale.toString()).toBe(`en-u-fw-${expected}`);
            }
        }
    });

    test("true is canonicalized to an empty extension value", () => {
        for (const tag of ["en", "en-u-fw-tue"]) {
            for (const value of [true, "true", "TRUE"]) {
                const locale = new Intl.Locale(tag, { firstDayOfWeek: value });
                expect(locale.firstDayOfWeek).toBe("");
                expect(locale.toString()).toBe("en-u-fw");
            }
        }
    });
});
