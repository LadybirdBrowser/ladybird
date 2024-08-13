describe("errors", () => {
    test("called without new", () => {
        expect(() => {
            Temporal.TimeZone();
        }).toThrowWithMessage(TypeError, "Temporal.TimeZone constructor must be called with 'new'");
    });

    test("Invalid time zone name", () => {
        expect(() => {
            new Temporal.TimeZone("foo");
        }).toThrowWithMessage(RangeError, "Invalid time zone name 'foo'");
    });

    test("Invalid numeric UTC offset", () => {
        // FIXME: Error message should probably say '...name or UTC offset ...' :^)
        expect(() => {
            new Temporal.TimeZone("0123456");
        }).toThrowWithMessage(RangeError, "Invalid time zone name '0123456'");

        expect(() => {
            new Temporal.TimeZone("23:59:59.9999999999");
        }).toThrowWithMessage(RangeError, "Invalid time zone name '23:59:59.9999999999'");

        expect(() => {
            new Temporal.TimeZone("\u221201");
        }).toThrowWithMessage(RangeError, "Invalid time zone name '\u221201'");
    });
});

describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Temporal.TimeZone).toHaveLength(1);
    });

    test("basic functionality", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        expect(timeZone.id).toBe("UTC");
        expect(typeof timeZone).toBe("object");
        expect(timeZone).toBeInstanceOf(Temporal.TimeZone);
        expect(Object.getPrototypeOf(timeZone)).toBe(Temporal.TimeZone.prototype);
    });

    test("canonicalizes time zone name", () => {
        const values = [
            ["UTC", "UTC"],
            ["Utc", "UTC"],
            ["utc", "UTC"],
            ["uTc", "UTC"],
            ["GMT", "UTC"],
            ["Etc/UTC", "UTC"],
            ["Etc/GMT", "UTC"],
            ["Etc/GMT+12", "Etc/GMT+12"],
            ["Etc/GMT-12", "Etc/GMT-12"],
            ["Europe/London", "Europe/London"],
            ["Australia/Canberra", "Australia/Sydney"],
        ];
        for (const [arg, expected] of values) {
            expect(new Temporal.TimeZone(arg).id).toBe(expected);
        }
    });

    test("numeric UTC offset", () => {
        const signs = [
            ["+", "+"],
            ["-", "-"],
        ];
        const values = [
            ["01", "01:00"],
            ["0123", "01:23"],
            ["012345", "01:23:45"],
            ["012345.6", "01:23:45.6"],
            ["012345.123", "01:23:45.123"],
            ["012345.123456789", "01:23:45.123456789"],
            ["012345,6", "01:23:45.6"],
            ["012345,123", "01:23:45.123"],
            ["012345,123456789", "01:23:45.123456789"],
            ["01:23", "01:23"],
            ["01:23:45", "01:23:45"],
            ["01:23:45.6", "01:23:45.6"],
            ["01:23:45.123", "01:23:45.123"],
            ["01:23:45.123456789", "01:23:45.123456789"],
            ["01:23:45,6", "01:23:45.6"],
            ["01:23:45,123", "01:23:45.123"],
            ["01:23:45,123456789", "01:23:45.123456789"],
            ["23:59:59.999999999", "23:59:59.999999999"],
        ];
        for (const [sign, expectedSign] of signs) {
            for (const [offset, expectedOffset] of values) {
                expect(new Temporal.TimeZone(`${sign}${offset}`).id).toBe(
                    `${expectedSign}${expectedOffset}`
                );
            }
        }
    });
});
