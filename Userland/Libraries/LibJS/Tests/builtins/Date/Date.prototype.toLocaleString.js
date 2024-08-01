describe("errors", () => {
    test("called on non-Date object", () => {
        expect(() => {
            Date.prototype.toLocaleString();
        }).toThrowWithMessage(TypeError, "Not an object of type Date");
    });

    test("called with value that cannot be converted to a number", () => {
        expect(() => {
            new Date(Symbol.hasInstance).toLocaleString();
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            new Date(1n).toLocaleString();
        }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");
    });
});

describe("correct behavior", () => {
    test("NaN", () => {
        const d = new Date(NaN);
        expect(d.toLocaleString()).toBe("Invalid Date");
    });

    test("time clip", () => {
        const d = new Date(-8.65e15);
        expect(d.toLocaleString()).toBe("Invalid Date");
    });

    const d0 = new Date(Date.UTC(2021, 11, 7, 17, 40, 50, 456));
    const d1 = new Date(Date.UTC(1989, 0, 23, 7, 8, 9, 45));

    test("defaults to date and time", () => {
        expect(d0.toLocaleString("en", { timeZone: "UTC" })).toBe("12/7/2021, 5:40:50 PM");
        expect(d1.toLocaleString("en", { timeZone: "UTC" })).toBe("1/23/1989, 7:08:09 AM");

        expect(d0.toLocaleString("ar", { timeZone: "UTC" })).toBe("٧‏/١٢‏/٢٠٢١، ٥:٤٠:٥٠ م");
        expect(d1.toLocaleString("ar", { timeZone: "UTC" })).toBe("٢٣‏/١‏/١٩٨٩، ٧:٠٨:٠٩ ص");
    });

    test("dateStyle may be set", () => {
        expect(d0.toLocaleString("en", { dateStyle: "short", timeZone: "UTC" })).toBe("12/7/21");
        expect(d1.toLocaleString("en", { dateStyle: "short", timeZone: "UTC" })).toBe("1/23/89");

        expect(d0.toLocaleString("ar", { dateStyle: "short", timeZone: "UTC" })).toBe(
            "٧‏/١٢‏/٢٠٢١"
        );
        expect(d1.toLocaleString("ar", { dateStyle: "short", timeZone: "UTC" })).toBe(
            "٢٣‏/١‏/١٩٨٩"
        );
    });

    test("timeStyle may be set", () => {
        expect(d0.toLocaleString("en", { timeStyle: "short", timeZone: "UTC" })).toBe("5:40 PM");
        expect(d1.toLocaleString("en", { timeStyle: "short", timeZone: "UTC" })).toBe("7:08 AM");

        expect(d0.toLocaleString("ar", { timeStyle: "short", timeZone: "UTC" })).toBe("٥:٤٠ م");
        expect(d1.toLocaleString("ar", { timeStyle: "short", timeZone: "UTC" })).toBe("٧:٠٨ ص");
    });
});
