describe("normal behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainDateTime.prototype.toPlainDate).toHaveLength(0);
    });

    test("basic functionality", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 7, 23, 0, 32, 18, 123, 456, 789);
        const plainDate = plainDateTime.toPlainDate();
        expect(plainDate.equals(new Temporal.PlainDate(2021, 7, 23))).toBeTrue();
    });
});
