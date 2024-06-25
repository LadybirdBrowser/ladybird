describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.TimeZone.prototype.getOffsetNanosecondsFor).toHaveLength(1);
    });

    test("basic functionality", () => {
        // Adapted from TestTimeZone.cpp's TEST_CASE(time_zone_offset).

        function offset(sign, hours, minutes, seconds) {
            return sign * (hours * 3600 + minutes * 60 + seconds) * 1_000_000_000;
        }

        function testOffset(timeZone, time, expectedOffset) {
            const instant = new Temporal.Instant(BigInt(time) * 1_000_000_000n);
            const actualOffset = new Temporal.TimeZone(timeZone).getOffsetNanosecondsFor(instant);
            expect(actualOffset).toBe(expectedOffset);
        }

        testOffset("America/Chicago", -2717647201, offset(-1, 5, 50, 36)); // November 18, 1883 5:59:59 PM UTC
        testOffset("America/Chicago", -2717647200, offset(-1, 6, 0, 0)); // November 18, 1883 6:00:00 PM UTC
        testOffset("America/Chicago", -1067788860, offset(-1, 6, 0, 0)); // March 1, 1936 1:59:00 AM Chicago (March 1, 1936 7:59:00 AM UTC)
        testOffset("America/Chicago", -1067788800, offset(-1, 5, 0, 0)); // March 1, 1936 3:00:00 AM Chicago (March 1, 1936 8:00:00 AM UTC)
        testOffset("America/Chicago", -1045414860, offset(-1, 5, 0, 0)); // November 15, 1936 1:59:00 AM Chicago (November 15, 1936 6:59:00 AM UTC)
        testOffset("America/Chicago", -1045411200, offset(-1, 6, 0, 0)); // November 15, 1936 2:00:00 AM Chicago (November 15, 1936 8:00:00 AM UTC)

        testOffset("Europe/London", -3852662326, offset(-1, 0, 1, 15)); // November 30, 1847 11:59:59 PM London (December 1, 1847 12:01:14 AM UTC)
        testOffset("Europe/London", -3852662325, offset(+1, 0, 0, 0)); // December 1, 1847 12:01:15 AM London (December 1, 1847 12:01:15 AM UTC)
        testOffset("Europe/London", -59004001, offset(+1, 0, 0, 0)); // February 18, 1968 1:59:59 AM London (February 18, 1968 1:59:59 AM UTC)
        testOffset("Europe/London", -59004000, offset(+1, 1, 0, 0)); // February 18, 1968 3:00:00 AM London (February 18, 1968 2:00:00 AM UTC)
        testOffset("Europe/London", 57722399, offset(+1, 1, 0, 0)); // October 31, 1971 1:59:59 AM UTC
        testOffset("Europe/London", 57722400, offset(+1, 0, 0, 0)); // October 31, 1971 2:00:00 AM UTC

        testOffset("UTC", -1641846268, offset(+1, 0, 0, 0));
        testOffset("UTC", 0, offset(+1, 0, 0, 0));
        testOffset("UTC", 1641846268, offset(+1, 0, 0, 0));

        testOffset("Etc/GMT+4", -1641846268, offset(-1, 4, 0, 0));
        testOffset("Etc/GMT+5", 0, offset(-1, 5, 0, 0));
        testOffset("Etc/GMT+6", 1641846268, offset(-1, 6, 0, 0));

        testOffset("Etc/GMT-12", -1641846268, offset(+1, 12, 0, 0));
        testOffset("Etc/GMT-13", 0, offset(+1, 13, 0, 0));
        testOffset("Etc/GMT-14", 1641846268, offset(+1, 14, 0, 0));
    });

    test("custom offset", () => {
        const timeZone = new Temporal.TimeZone("+01:30");
        const instant = new Temporal.Instant(0n);
        expect(timeZone.getOffsetNanosecondsFor(instant)).toBe(5400000000000);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.TimeZone object", () => {
        expect(() => {
            Temporal.TimeZone.prototype.getOffsetNanosecondsFor.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.TimeZone");
    });
});
