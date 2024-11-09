describe("correct behavior", () => {
    test("length is 2", () => {
        expect(Temporal.Calendar.prototype.mergeFields).toHaveLength(2);
    });

    test("basic functionality", () => {
        const calendar = new Temporal.Calendar("iso8601");
        const fields = {};
        const additionalFields = {};
        const mergedFields = calendar.mergeFields(fields, additionalFields);
        expect(mergedFields).not.toBe(fields);
        expect(mergedFields).not.toBe(additionalFields);
        expect(mergedFields).toEqual({});
        expect(Object.getPrototypeOf(mergedFields)).toBe(Object.prototype);
        expect(calendar.mergeFields({ foo: 1, bar: 1 }, { foo: 2 })).toEqual({ foo: 2, bar: 1 });
        expect(calendar.mergeFields({ foo: 1 }, { foo: 2, bar: 2 })).toEqual({ foo: 2, bar: 2 });
        expect(calendar.mergeFields({ foo: 1 }, { foo: 2, bar: 2 })).toEqual({ foo: 2, bar: 2 });
    });

    test("month and monthCode property handling", () => {
        const calendar = new Temporal.Calendar("iso8601");
        expect(calendar.mergeFields({ month: 1 }, { monthCode: 2 })).toEqual({ monthCode: 2 });
        expect(calendar.mergeFields({ monthCode: 1 }, { month: 2 })).toEqual({ month: 2 });
        expect(calendar.mergeFields({ month: 1, monthCode: 1 }, {})).toEqual({
            month: 1,
            monthCode: 1,
        });
        expect(
            calendar.mergeFields({ month: 1, monthCode: 1 }, { month: 2, monthCode: 2 })
        ).toEqual({ month: 2, monthCode: 2 });
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Calendar object", () => {
        expect(() => {
            Temporal.Calendar.prototype.mergeFields.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Calendar");
    });

    test("fields argument must be coercible to object", () => {
        const calendar = new Temporal.Calendar("iso8601");
        expect(() => {
            calendar.mergeFields(null, {});
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
    });

    test("additionalFields argument must be coercible to object", () => {
        const calendar = new Temporal.Calendar("iso8601");
        expect(() => {
            calendar.mergeFields({}, null);
        }).toThrowWithMessage(TypeError, "ToObject on null or undefined");
    });
});
