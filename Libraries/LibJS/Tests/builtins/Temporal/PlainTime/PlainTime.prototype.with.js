const PLAIN_TIME_PROPERTIES = [
    "hour",
    "minute",
    "second",
    "millisecond",
    "microsecond",
    "nanosecond",
];

const REJECTED_CALENDAR_TYPES_THREE_ARGUMENTS = [
    Temporal.PlainDate,
    Temporal.PlainDateTime,
    Temporal.PlainTime,
];

const REJECTED_CALENDAR_TYPES_TWO_ARGUMENTS = [Temporal.PlainMonthDay, Temporal.PlainYearMonth];

describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainTime.prototype.with).toHaveLength(1);
    });

    test("basic functionality", () => {
        const plainTime = new Temporal.PlainTime(1, 2, 3).with({ hour: 4, foo: 5, second: 6 });
        expect(plainTime.hour).toBe(4);
        expect(plainTime.minute).toBe(2);
        expect(plainTime.second).toBe(6);
    });

    test("each property is looked up from the object", () => {
        for (const property of PLAIN_TIME_PROPERTIES) {
            const plainTime = new Temporal.PlainTime().with({ [property]: 1 });
            expect(plainTime[property]).toBe(1);
        }
    });

    test("each property is coerced to number", () => {
        for (const property of PLAIN_TIME_PROPERTIES) {
            const plainTime = new Temporal.PlainTime().with({ [property]: "1" });
            expect(plainTime[property]).toBe(1);
        }
    });

    test("argument can have a calendar property as long as it's undefined", () => {
        expect(() => {
            new Temporal.PlainTime().with({
                calendar: undefined,
            });
        }).not.toThrowWithMessage(TypeError, "Object must not have a defined calendar property");
    });

    test("argument can have a timeZone property as long as it's undefined", () => {
        expect(() => {
            new Temporal.PlainTime().with({
                timeZone: undefined,
            });
        }).not.toThrowWithMessage(TypeError, "Object must not have a defined timeZone property");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainTime object", () => {
        expect(() => {
            Temporal.PlainTime.prototype.with.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainTime");
    });

    test("argument is not an object", () => {
        expect(() => {
            new Temporal.PlainTime().with("foo");
        }).toThrowWithMessage(TypeError, "foo is not an object");
        expect(() => {
            new Temporal.PlainTime().with(42);
        }).toThrowWithMessage(TypeError, "42 is not an object");
    });

    test("options is not an object", () => {
        expect(() => {
            new Temporal.PlainTime().with({ hour: 1 }, "foo");
        }).toThrowWithMessage(TypeError, "Options is not an object");
        expect(() => {
            new Temporal.PlainTime().with({ hour: 1 }, 42);
        }).toThrowWithMessage(TypeError, "Options is not an object");
    });

    test("invalid overflow option", () => {
        expect(() => {
            new Temporal.PlainTime().with({ hour: 1 }, { overflow: "a" });
        }).toThrowWithMessage(RangeError, "a is not a valid value for option overflow");
    });

    test("argument is an invalid plain time-like object", () => {
        expect(() => {
            new Temporal.PlainTime().with({});
        }).toThrowWithMessage(
            TypeError,
            "Object must have at least one of the following properties: hour, microsecond, millisecond, minute, nanosecond, second"
        );
        expect(() => {
            new Temporal.PlainTime().with({ foo: 1, bar: 2 });
        }).toThrowWithMessage(
            TypeError,
            "Object must have at least one of the following properties: hour, microsecond, millisecond, minute, nanosecond, second"
        );
    });

    test("error when coercing property to number", () => {
        for (const property of PLAIN_TIME_PROPERTIES) {
            expect(() => {
                new Temporal.PlainTime().with({
                    [property]: {
                        valueOf() {
                            throw new Error("error occurred");
                        },
                    },
                });
            }).toThrowWithMessage(Error, "error occurred");
        }
    });

    test("property must be finite", () => {
        for (const property of PLAIN_TIME_PROPERTIES) {
            expect(() => {
                new Temporal.PlainTime().with({ [property]: Infinity });
            }).toThrowWithMessage(RangeError, "Property must not be Infinity");
            expect(() => {
                new Temporal.PlainTime().with({ [property]: -Infinity });
            }).toThrowWithMessage(RangeError, "Property must not be Infinity");
        }
    });

    test("error when getting property", () => {
        for (const property of PLAIN_TIME_PROPERTIES) {
            expect(() => {
                new Temporal.PlainTime().with({
                    get [property]() {
                        throw new Error("error occurred");
                    },
                });
            }).toThrowWithMessage(Error, "error occurred");
        }
    });

    test("argument must not have a defined calendar property", () => {
        expect(() => {
            new Temporal.PlainTime().with({
                calendar: null,
            });
        }).toThrowWithMessage(TypeError, "Object must not have a defined calendar property");
        expect(() => {
            new Temporal.PlainTime().with({
                calendar: 1,
            });
        }).toThrowWithMessage(TypeError, "Object must not have a defined calendar property");
    });

    test("argument must not have a defined timeZone property", () => {
        expect(() => {
            new Temporal.PlainTime().with({
                timeZone: null,
            });
        }).toThrowWithMessage(TypeError, "Object must not have a defined timeZone property");
        expect(() => {
            new Temporal.PlainTime().with({
                timeZone: 1,
            });
        }).toThrowWithMessage(TypeError, "Object must not have a defined timeZone property");
    });

    test("error when getting calendar", () => {
        expect(() => {
            new Temporal.PlainTime().with({
                get calendar() {
                    throw new Error("error occurred");
                },
            });
        }).toThrowWithMessage(Error, "error occurred");
    });

    test("error when getting timeZone", () => {
        expect(() => {
            new Temporal.PlainTime().with({
                get timeZone() {
                    throw new Error("error occurred");
                },
            });
        }).toThrowWithMessage(Error, "error occurred");
    });

    test("rejects calendar types", () => {
        for (const typeWithCalendar of REJECTED_CALENDAR_TYPES_THREE_ARGUMENTS) {
            expect(() => {
                new Temporal.PlainTime().with(new typeWithCalendar(1, 1, 1));
            }).toThrowWithMessage(
                TypeError,
                "Object must not have a defined calendar or timeZone property"
            );
        }

        for (const typeWithCalendar of REJECTED_CALENDAR_TYPES_TWO_ARGUMENTS) {
            expect(() => {
                new Temporal.PlainTime().with(new typeWithCalendar(1, 1));
            }).toThrowWithMessage(
                TypeError,
                "Object must not have a defined calendar or timeZone property"
            );
        }

        expect(() => {
            new Temporal.PlainTime().with(new Temporal.ZonedDateTime(1n, {}));
        }).toThrowWithMessage(
            TypeError,
            "Object must not have a defined calendar or timeZone property"
        );
    });
});
