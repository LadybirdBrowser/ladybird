describe("correct behavior", () => {
    test("basic functionality", () => {
        const plainDate = new Temporal.PlainDate(2021, 7, 6);
        expect(plainDate.era).toBeUndefined();
    });

    test("undefined for iso8601 calendar", () => {
        const plainDate = new Temporal.PlainDate(2024, 1, 1);
        expect(plainDate.era).toBeUndefined();
    });

    test("gregory calendar", () => {
        const ce = new Temporal.PlainDate(2024, 1, 1, "gregory");
        expect(ce.era).toBe("ce");

        const bce = new Temporal.PlainDate(-100, 1, 1, "gregory");
        expect(bce.era).toBe("bce");
    });

    test("japanese calendar eras", () => {
        const reiwa = new Temporal.PlainDate(2020, 1, 1, "japanese");
        expect(reiwa.era).toBe("reiwa");

        const heisei = new Temporal.PlainDate(2000, 1, 1, "japanese");
        expect(heisei.era).toBe("heisei");

        const showa = new Temporal.PlainDate(1970, 1, 1, "japanese");
        expect(showa.era).toBe("showa");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainDate.prototype, "era", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
    });
});
