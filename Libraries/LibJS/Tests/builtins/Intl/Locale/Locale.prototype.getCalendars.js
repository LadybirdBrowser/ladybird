describe("errors", () => {
    test("called on non-Locale object", () => {
        expect(() => {
            Intl.Locale.prototype.getCalendars();
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.Locale");
    });
});

describe("normal behavior", () => {
    const testCalendars = (locale, expected) => {
        const result = locale.getCalendars();
        expect(Array.isArray(result)).toBeTrue();

        for (const entry of expected) {
            expect(result).toContain(entry);
        }
    };

    test("basic functionality", () => {
        testCalendars(new Intl.Locale("en"), ["gregory"]);
        testCalendars(new Intl.Locale("ar"), ["gregory"]);
    });

    test("extension keyword overrides default data", () => {
        testCalendars(new Intl.Locale("en-u-ca-islamicc"), ["islamic-civil"]);
        testCalendars(new Intl.Locale("en", { calendar: "dangi" }), ["dangi"]);

        testCalendars(new Intl.Locale("ar-u-ca-ethiopic-amete-alem"), ["ethioaa"]);
        testCalendars(new Intl.Locale("ar", { calendar: "hebrew" }), ["hebrew"]);

        // Invalid calendars also take precedence.
        testCalendars(new Intl.Locale("en-u-ca-ladybird"), ["ladybird"]);
        testCalendars(new Intl.Locale("en", { calendar: "ladybird" }), ["ladybird"]);
    });
});
