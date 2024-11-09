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
            }).toThrowWithMessage(
                RangeError,
                `${value} is not a valid value for option firstDayOfWeek`
            );
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

            expect(new Intl.Locale("en-u-fw-mon", { firstDayOfWeek: day }).firstDayOfWeek).toBe(
                day
            );
            expect(new Intl.Locale("en-u-fw-mon", { firstDayOfWeek: index }).firstDayOfWeek).toBe(
                day
            );
        });
    });

    test("non-standard options", () => {
        [100, Infinity, NaN, "hello", 152n, true].forEach(value => {
            expect(new Intl.Locale("en", { firstDayOfWeek: value }).firstDayOfWeek).toBe(
                value.toString()
            );
        });
    });
});
