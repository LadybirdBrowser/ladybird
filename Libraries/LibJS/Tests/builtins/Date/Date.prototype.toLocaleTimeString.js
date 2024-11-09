describe("errors", () => {
    test("called on non-Date object", () => {
        expect(() => {
            Date.prototype.toLocaleTimeString();
        }).toThrowWithMessage(TypeError, "Not an object of type Date");
    });

    test("called with value that cannot be converted to a number", () => {
        expect(() => {
            new Date(Symbol.hasInstance).toLocaleTimeString();
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            new Date(1n).toLocaleTimeString();
        }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");
    });

    test("dateStyle may not be specified", () => {
        expect(() => {
            new Date().toLocaleTimeString([], { dateStyle: "short" });
        }).toThrowWithMessage(TypeError, "Option dateStyle cannot be set when also providing time");
    });
});

describe("correct behavior", () => {
    test("NaN", () => {
        const d = new Date(NaN);
        expect(d.toLocaleTimeString()).toBe("Invalid Date");
    });

    test("time clip", () => {
        const d = new Date(-8.65e15);
        expect(d.toLocaleTimeString()).toBe("Invalid Date");
    });

    const d0 = new Date(Date.UTC(2021, 11, 7, 17, 40, 50, 456));
    const d1 = new Date(Date.UTC(1989, 0, 23, 7, 8, 9, 45));

    test("defaults to time", () => {
        expect(d0.toLocaleTimeString("en", { timeZone: "UTC" })).toBe("5:40:50 PM");
        expect(d1.toLocaleTimeString("en", { timeZone: "UTC" })).toBe("7:08:09 AM");

        expect(d0.toLocaleTimeString("ar", { timeZone: "UTC" })).toBe("٥:٤٠:٥٠ م");
        expect(d1.toLocaleTimeString("ar", { timeZone: "UTC" })).toBe("٧:٠٨:٠٩ ص");
    });

    test("timeStyle may be set", () => {
        expect(d0.toLocaleTimeString("en", { timeStyle: "long", timeZone: "UTC" })).toBe(
            "5:40:50 PM UTC"
        );
        expect(d1.toLocaleTimeString("en", { timeStyle: "long", timeZone: "UTC" })).toBe(
            "7:08:09 AM UTC"
        );

        expect(d0.toLocaleTimeString("ar", { timeStyle: "long", timeZone: "UTC" })).toBe(
            "٥:٤٠:٥٠ م UTC"
        );
        expect(d1.toLocaleTimeString("ar", { timeStyle: "long", timeZone: "UTC" })).toBe(
            "٧:٠٨:٠٩ ص UTC"
        );
    });
});
