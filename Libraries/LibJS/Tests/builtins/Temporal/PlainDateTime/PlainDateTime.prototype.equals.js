describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainDateTime.prototype.equals).toHaveLength(1);
    });

    test("basic functionality", () => {
        const calendar = { hello: "friends" };
        const firstPlainDateTime = new Temporal.PlainDateTime(1, 1, 1, 1, 1, 1, 1, 1, 1, calendar);
        const secondPlainDateTime = new Temporal.PlainDateTime(0, 1, 1, 1, 1, 1, 1, 1, 1, calendar);
        expect(firstPlainDateTime.equals(firstPlainDateTime));
        expect(!firstPlainDateTime.equals(secondPlainDateTime));
    });
});
