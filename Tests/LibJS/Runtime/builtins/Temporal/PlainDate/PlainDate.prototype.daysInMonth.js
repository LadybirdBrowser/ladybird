describe("correct behavior", () => {
    test("basic functionality", () => {
        const date = new Temporal.PlainDate(2021, 7, 23);
        expect(date.daysInMonth).toBe(31);
    });

    test("islamic calendar months alternate 30 and 29 days", () => {
        // Islamic month 1 (Muharram) has 30 days.
        const plainDate = Temporal.PlainDate.from("2024-07-08[u-ca=islamic-civil]");
        expect(plainDate.month).toBe(1);
        expect(plainDate.daysInMonth).toBe(30);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainDate.prototype, "daysInMonth", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
    });
});
