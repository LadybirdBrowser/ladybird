describe("correct behavior", () => {
    test("basic functionality", () => {
        const date = new Temporal.PlainDate(2021, 7, 23);
        expect(date.inLeapYear).toBeFalse();
        const leapDate = new Temporal.PlainDate(2020, 7, 23);
        expect(leapDate.inLeapYear).toBeTrue();
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainDate.prototype, "inLeapYear", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
    });
});
