describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.ZonedDateTime.prototype.getTimeZoneTransition).toHaveLength(1);
    });

    test("returns null for UTC time zone", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(1627318123456789000n, "UTC", "iso8601");
        expect(zonedDateTime.getTimeZoneTransition("next")).toBeNull();
        expect(zonedDateTime.getTimeZoneTransition("previous")).toBeNull();
    });

    test("returns null for offset time zones", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(1627318123456789000n, "+05:30", "iso8601");
        expect(zonedDateTime.getTimeZoneTransition("next")).toBeNull();
        expect(zonedDateTime.getTimeZoneTransition("previous")).toBeNull();

        const zonedDateTime2 = new Temporal.ZonedDateTime(1627318123456789000n, "-08:00", "iso8601");
        expect(zonedDateTime2.getTimeZoneTransition("next")).toBeNull();
        expect(zonedDateTime2.getTimeZoneTransition("previous")).toBeNull();
    });

    test("finds next DST transition", () => {
        // 2020-01-15 in America/Los_Angeles, before spring DST transition
        const zdt = Temporal.ZonedDateTime.from("2020-01-15T12:00:00-08:00[America/Los_Angeles]");
        const nextTransition = zdt.getTimeZoneTransition("next");

        expect(nextTransition).not.toBeNull();

        // DST starts on March 8, 2020 at 2:00 AM, clocks spring forward to 3:00 AM
        expect(nextTransition.toString()).toBe("2020-03-08T03:00:00-07:00[America/Los_Angeles]");
    });

    test("finds previous DST transition", () => {
        // 2020-06-15 in America/Los_Angeles, after spring DST transition
        const zdt = Temporal.ZonedDateTime.from("2020-06-15T12:00:00-07:00[America/Los_Angeles]");
        const prevTransition = zdt.getTimeZoneTransition("previous");

        expect(prevTransition).not.toBeNull();

        // DST started on March 8, 2020 at 2:00 AM -> 3:00 AM
        expect(prevTransition.toString()).toBe("2020-03-08T03:00:00-07:00[America/Los_Angeles]");
    });

    test("direction can be passed as string", () => {
        const zdt = Temporal.ZonedDateTime.from("2020-06-15T12:00:00-07:00[America/Los_Angeles]");

        const nextTransition = zdt.getTimeZoneTransition("next");
        expect(nextTransition).not.toBeNull();

        // DST ends on November 1, 2020 at 2:00 AM -> 1:00 AM
        expect(nextTransition.toString()).toBe("2020-11-01T01:00:00-08:00[America/Los_Angeles]");

        const prevTransition = zdt.getTimeZoneTransition("previous");
        expect(prevTransition).not.toBeNull();
        expect(prevTransition.toString()).toBe("2020-03-08T03:00:00-07:00[America/Los_Angeles]");
    });

    test("direction can be passed as object with direction property", () => {
        const zdt = Temporal.ZonedDateTime.from("2020-06-15T12:00:00-07:00[America/Los_Angeles]");

        const nextTransition = zdt.getTimeZoneTransition({ direction: "next" });
        expect(nextTransition).not.toBeNull();
        expect(nextTransition.toString()).toBe("2020-11-01T01:00:00-08:00[America/Los_Angeles]");

        const prevTransition = zdt.getTimeZoneTransition({ direction: "previous" });
        expect(prevTransition).not.toBeNull();
        expect(prevTransition.toString()).toBe("2020-03-08T03:00:00-07:00[America/Los_Angeles]");
    });

    test("preserves calendar from original ZonedDateTime", () => {
        const zdt = Temporal.ZonedDateTime.from("2020-01-15T12:00:00-08:00[America/Los_Angeles][u-ca=gregory]");
        const nextTransition = zdt.getTimeZoneTransition("next");

        expect(nextTransition).not.toBeNull();
        expect(nextTransition.calendarId).toBe("gregory");
    });

    test("transition represents first nanosecond of new offset", () => {
        // Before DST ends in fall
        const zdt = Temporal.ZonedDateTime.from("2020-10-15T12:00:00-07:00[America/Los_Angeles]");
        const transition = zdt.getTimeZoneTransition("next");

        expect(transition).not.toBeNull();

        // The transition should be at the first nanosecond of the new offset
        // DST ends at 2:00 AM PDT, clocks fall back to 1:00 AM PST
        expect(transition.offset).toBe("-08:00");

        // One nanosecond before should be the old offset
        const beforeTransition = transition.subtract({ nanoseconds: 1 });
        expect(beforeTransition.offset).toBe("-07:00");
    });

    test("only returns transitions where UTC offset changes (next)", () => {
        // Europe/Moscow has a non-offset-changing transition on 1991-03-31 (a rule change
        // where the offset stayed at +03:00). The getTimeZoneTransition method should skip
        // this and return the next offset-changing transition on 1991-09-29 instead.
        const zdt = Temporal.ZonedDateTime.from("1991-01-01T00:00:00+03:00[Europe/Moscow]");
        const nextTransition = zdt.getTimeZoneTransition("next");

        expect(nextTransition).not.toBeNull();

        // Should skip 1991-03-31T02:00:00+03:00 (no offset change) and return 1991-09-29
        expect(nextTransition.toString()).toBe("1991-09-29T02:00:00+02:00[Europe/Moscow]");

        // Verify the offset actually changed
        const beforeTransition = nextTransition.subtract({ nanoseconds: 1 });
        expect(beforeTransition.offset).toBe("+03:00");
        expect(nextTransition.offset).toBe("+02:00");
    });

    test("only returns transitions where UTC offset changes (previous)", () => {
        // Going backwards from the 1991-09-29 transition (where offset changed to +02:00),
        // there's a non-offset-changing transition on 1991-03-31. The getTimeZoneTransition
        // method should skip this and return the previous offset-changing transition on
        // 1990-09-30 instead.
        const zdt = Temporal.ZonedDateTime.from("1991-09-29T02:00:00+02:00[Europe/Moscow]");
        const prevTransition = zdt.getTimeZoneTransition("previous");

        expect(prevTransition).not.toBeNull();

        // Should skip 1991-03-31T02:00:00+03:00 (no offset change) and return 1990-09-30
        expect(prevTransition.toString()).toBe("1990-09-30T02:00:00+03:00[Europe/Moscow]");

        // Verify the offset actually changed
        const beforeTransition = prevTransition.subtract({ nanoseconds: 1 });
        expect(beforeTransition.offset).toBe("+04:00");
        expect(prevTransition.offset).toBe("+03:00");
    });

    test("returns null when no more transitions in direction", () => {
        // America/Regina abolished DST in 1966
        const zdt = Temporal.ZonedDateTime.from("2020-06-15T12:00:00-06:00[America/Regina]");
        const nextTransition = zdt.getTimeZoneTransition("next");
        expect(nextTransition).toBeNull();
    });

    test("handles time zones with historical transitions only", () => {
        // America/Regina has past transitions but no future ones
        const zdt = Temporal.ZonedDateTime.from("2020-06-15T12:00:00-06:00[America/Regina]");
        const prevTransition = zdt.getTimeZoneTransition("previous");

        // Should find the last historical transition (DST was abolished after April 1960)
        expect(prevTransition).not.toBeNull();
        expect(prevTransition.toString()).toBe("1960-04-24T03:00:00-06:00[America/Regina]");
    });

    test("handles Europe/London transitions", () => {
        // 2020-06-15 in Europe/London during BST (British Summer Time)
        const zdt = Temporal.ZonedDateTime.from("2020-06-15T12:00:00+01:00[Europe/London]");

        const nextTransition = zdt.getTimeZoneTransition("next");
        expect(nextTransition).not.toBeNull();
        // BST ends on October 25, 2020 at 2:00 AM -> 1:00 AM GMT
        expect(nextTransition.toString()).toBe("2020-10-25T01:00:00+00:00[Europe/London]");

        const prevTransition = zdt.getTimeZoneTransition("previous");
        expect(prevTransition).not.toBeNull();
        // BST started on March 29, 2020 at 1:00 AM -> 2:00 AM
        expect(prevTransition.toString()).toBe("2020-03-29T02:00:00+01:00[Europe/London]");
    });

    test("previous transition with sub-millisecond precision", () => {
        // Edge case: when at a point just past a transition with sub-millisecond precision,
        // getTimeZoneTransition("previous") should still find that transition.
        // For example, if there's a transition at exactly T, and we're at T + 100 nanoseconds,
        // the previous transition should be the one at T, not an earlier one.
        const zdt = Temporal.ZonedDateTime.from("2020-01-01T00:00:00-08:00[America/Los_Angeles]");

        // Find the next transition
        const transition = zdt.getTimeZoneTransition("next");
        expect(transition).not.toBeNull();
        expect(transition.toString()).toBe("2020-03-08T03:00:00-07:00[America/Los_Angeles]");

        // Add sub-millisecond precision (100 nanoseconds past the transition)
        const justPastTransition = transition.add({ nanoseconds: 100 });
        expect(justPastTransition.epochNanoseconds).toBe(transition.epochNanoseconds + 100n);

        // Finding the previous transition from just past should return the same transition
        const prevFromJustPast = justPastTransition.getTimeZoneTransition("previous");
        expect(prevFromJustPast).not.toBeNull();
        expect(prevFromJustPast.epochNanoseconds).toBe(transition.epochNanoseconds);
    });

    test("chaining transitions", () => {
        const zdt = Temporal.ZonedDateTime.from("2020-01-01T00:00:00-08:00[America/Los_Angeles]");

        // Find the first transition
        const first = zdt.getTimeZoneTransition("next");
        expect(first).not.toBeNull();
        expect(first.toString()).toBe("2020-03-08T03:00:00-07:00[America/Los_Angeles]");

        // Find the next transition from that point
        const second = first.getTimeZoneTransition("next");
        expect(second).not.toBeNull();
        expect(second.toString()).toBe("2020-11-01T01:00:00-08:00[America/Los_Angeles]");

        // Find the next transition from that point
        const third = second.getTimeZoneTransition("next");
        expect(third).not.toBeNull();
        expect(third.toString()).toBe("2021-03-14T03:00:00-07:00[America/Los_Angeles]");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Temporal.ZonedDateTime.prototype.getTimeZoneTransition.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });

    test("direction parameter is required", () => {
        const zdt = new Temporal.ZonedDateTime(1627318123456789000n, "America/Los_Angeles");
        expect(() => {
            zdt.getTimeZoneTransition();
        }).toThrowWithMessage(TypeError, "Transition direction parameter is undefined");

        expect(() => {
            zdt.getTimeZoneTransition(undefined);
        }).toThrowWithMessage(TypeError, "Transition direction parameter is undefined");
    });

    test("invalid direction string throws RangeError", () => {
        const zdt = new Temporal.ZonedDateTime(1627318123456789000n, "America/Los_Angeles");
        expect(() => {
            zdt.getTimeZoneTransition("invalid");
        }).toThrowWithMessage(RangeError, "invalid is not a valid value for option direction");

        expect(() => {
            zdt.getTimeZoneTransition("forward");
        }).toThrowWithMessage(RangeError, "forward is not a valid value for option direction");

        expect(() => {
            zdt.getTimeZoneTransition("backward");
        }).toThrowWithMessage(RangeError, "backward is not a valid value for option direction");
    });

    test("invalid direction in object throws RangeError", () => {
        const zdt = new Temporal.ZonedDateTime(1627318123456789000n, "America/Los_Angeles");
        expect(() => {
            zdt.getTimeZoneTransition({ direction: "invalid" });
        }).toThrowWithMessage(RangeError, "invalid is not a valid value for option direction");
    });

    test("null direction throws", () => {
        const zdt = new Temporal.ZonedDateTime(1627318123456789000n, "America/Los_Angeles");
        expect(() => {
            zdt.getTimeZoneTransition(null);
        }).toThrowWithMessage(TypeError, "Options is not an object");
    });
});
