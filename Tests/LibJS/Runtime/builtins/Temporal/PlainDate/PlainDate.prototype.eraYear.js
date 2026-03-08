describe("correct behavior", () => {
    test("basic functionality", () => {
        const plainDate = new Temporal.PlainDate(2021, 7, 6);
        expect(plainDate.eraYear).toBeUndefined();
    });

    test("undefined for iso8601 calendar", () => {
        const plainDate = new Temporal.PlainDate(2024, 1, 1);
        expect(plainDate.eraYear).toBeUndefined();
    });

    test("gregory calendar", () => {
        const plainDate = new Temporal.PlainDate(2024, 1, 1, "gregory");
        expect(plainDate.eraYear).toBe(2024);
    });

    test("japanese calendar era year", () => {
        const reiwa = new Temporal.PlainDate(2024, 6, 15, "japanese");
        expect(reiwa.eraYear).toBe(6);

        const heisei = new Temporal.PlainDate(2000, 1, 1, "japanese");
        expect(heisei.eraYear).toBe(12);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainDate.prototype, "eraYear", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
    });
});
