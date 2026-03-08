describe("correct behavior", () => {
    test("basic functionality", () => {
        const date = new Temporal.PlainDate(2021, 7, 23);
        expect(date.monthsInYear).toBe(12);
    });

    test("hebrew leap year has 13 months", () => {
        const plainDate = Temporal.PlainDate.from("2024-04-09[u-ca=hebrew]");
        expect(plainDate.year).toBe(5784);
        expect(plainDate.monthsInYear).toBe(13);
    });

    test("hebrew non-leap year has 12 months", () => {
        const plainDate = Temporal.PlainDate.from("2023-04-09[u-ca=hebrew]");
        expect(plainDate.year).toBe(5783);
        expect(plainDate.monthsInYear).toBe(12);
    });

    test("chinese leap year has 13 months", () => {
        // 2023 has a Chinese leap month (leap M02), so Chinese year 4660 has 13 months.
        const plainDate = Temporal.PlainDate.from("2023-03-01[u-ca=chinese]");
        expect(plainDate.monthsInYear).toBe(13);
    });

    test("chinese non-leap year has 12 months", () => {
        // 2024 Chinese year 4721 is not a leap year.
        const plainDate = Temporal.PlainDate.from("2024-06-01[u-ca=chinese]");
        expect(plainDate.monthsInYear).toBe(12);
    });

    test("coptic calendar always has 13 months", () => {
        const plainDate = Temporal.PlainDate.from("2024-01-01[u-ca=coptic]");
        expect(plainDate.monthsInYear).toBe(13);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainDate.prototype, "monthsInYear", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
    });
});
