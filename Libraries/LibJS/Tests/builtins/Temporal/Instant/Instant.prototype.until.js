describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Instant.prototype.until).toHaveLength(1);
    });

    test("basic functionality", () => {
        const instant1 = new Temporal.Instant(0n);
        const instant2 = new Temporal.Instant(1625614920000000000n);
        expect(instant1.until(instant2).seconds).toBe(1625614920);
        expect(instant1.until(instant2, { largestUnit: "hour" }).hours).toBe(451559);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Instant object", () => {
        expect(() => {
            Temporal.Instant.prototype.until.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Instant");
    });
});

describe("rounding modes", () => {
    const earlier = new Temporal.Instant(
        217178610_123_456_789n /* 1976-11-18T15:23:30.123456789Z */
    );
    const later = new Temporal.Instant(
        1572345998_271_986_289n /* 2019-10-29T10:46:38.271986289Z */
    );
    const largestUnit = "hours";

    test("'ceil' rounding mode", () => {
        const expected = [
            ["hours", "PT376436H", "-PT376435H"],
            ["minutes", "PT376435H24M", "-PT376435H23M"],
            ["seconds", "PT376435H23M9S", "-PT376435H23M8S"],
            ["milliseconds", "PT376435H23M8.149S", "-PT376435H23M8.148S"],
            ["microseconds", "PT376435H23M8.14853S", "-PT376435H23M8.148529S"],
            ["nanoseconds", "PT376435H23M8.1485295S", "-PT376435H23M8.1485295S"],
        ];

        const roundingMode = "ceil";
        expected.forEach(([smallestUnit, expectedPositive, expectedNegative]) => {
            const untilPositive = earlier.until(later, { largestUnit, smallestUnit, roundingMode });
            expect(untilPositive.toString()).toBe(expectedPositive);

            const untilNegative = later.until(earlier, { largestUnit, smallestUnit, roundingMode });
            expect(untilNegative.toString()).toBe(expectedNegative);
        });
    });

    test("'expand' rounding mode", () => {
        const expected = [
            ["hours", "PT376436H", "-PT376436H"],
            ["minutes", "PT376435H24M", "-PT376435H24M"],
            ["seconds", "PT376435H23M9S", "-PT376435H23M9S"],
            ["milliseconds", "PT376435H23M8.149S", "-PT376435H23M8.149S"],
            ["microseconds", "PT376435H23M8.14853S", "-PT376435H23M8.14853S"],
            ["nanoseconds", "PT376435H23M8.1485295S", "-PT376435H23M8.1485295S"],
        ];

        const roundingMode = "expand";
        expected.forEach(([smallestUnit, expectedPositive, expectedNegative]) => {
            const untilPositive = earlier.until(later, { largestUnit, smallestUnit, roundingMode });
            expect(untilPositive.toString()).toBe(expectedPositive);

            const untilNegative = later.until(earlier, { largestUnit, smallestUnit, roundingMode });
            expect(untilNegative.toString()).toBe(expectedNegative);
        });
    });

    test("'floor' rounding mode", () => {
        const expected = [
            ["hours", "PT376435H", "-PT376436H"],
            ["minutes", "PT376435H23M", "-PT376435H24M"],
            ["seconds", "PT376435H23M8S", "-PT376435H23M9S"],
            ["milliseconds", "PT376435H23M8.148S", "-PT376435H23M8.149S"],
            ["microseconds", "PT376435H23M8.148529S", "-PT376435H23M8.14853S"],
            ["nanoseconds", "PT376435H23M8.1485295S", "-PT376435H23M8.1485295S"],
        ];

        const roundingMode = "floor";
        expected.forEach(([smallestUnit, expectedPositive, expectedNegative]) => {
            const untilPositive = earlier.until(later, { largestUnit, smallestUnit, roundingMode });
            expect(untilPositive.toString()).toBe(expectedPositive);

            const untilNegative = later.until(earlier, { largestUnit, smallestUnit, roundingMode });
            expect(untilNegative.toString()).toBe(expectedNegative);
        });
    });

    test("'halfCeil' rounding mode", () => {
        const expected = [
            ["hours", "PT376435H", "-PT376435H"],
            ["minutes", "PT376435H23M", "-PT376435H23M"],
            ["seconds", "PT376435H23M8S", "-PT376435H23M8S"],
            ["milliseconds", "PT376435H23M8.149S", "-PT376435H23M8.149S"],
            ["microseconds", "PT376435H23M8.14853S", "-PT376435H23M8.148529S"],
            ["nanoseconds", "PT376435H23M8.1485295S", "-PT376435H23M8.1485295S"],
        ];

        const roundingMode = "halfCeil";
        expected.forEach(([smallestUnit, expectedPositive, expectedNegative]) => {
            const untilPositive = earlier.until(later, { largestUnit, smallestUnit, roundingMode });
            expect(untilPositive.toString()).toBe(expectedPositive);

            const untilNegative = later.until(earlier, { largestUnit, smallestUnit, roundingMode });
            expect(untilNegative.toString()).toBe(expectedNegative);
        });
    });

    test("'halfEven' rounding mode", () => {
        const expected = [
            ["hours", "PT376435H", "-PT376435H"],
            ["minutes", "PT376435H23M", "-PT376435H23M"],
            ["seconds", "PT376435H23M8S", "-PT376435H23M8S"],
            ["milliseconds", "PT376435H23M8.149S", "-PT376435H23M8.149S"],
            ["microseconds", "PT376435H23M8.14853S", "-PT376435H23M8.14853S"],
            ["nanoseconds", "PT376435H23M8.1485295S", "-PT376435H23M8.1485295S"],
        ];

        const roundingMode = "halfEven";
        expected.forEach(([smallestUnit, expectedPositive, expectedNegative]) => {
            const untilPositive = earlier.until(later, { largestUnit, smallestUnit, roundingMode });
            expect(untilPositive.toString()).toBe(expectedPositive);

            const untilNegative = later.until(earlier, { largestUnit, smallestUnit, roundingMode });
            expect(untilNegative.toString()).toBe(expectedNegative);
        });
    });

    test("'halfExpand' rounding mode", () => {
        const expected = [
            ["hours", "PT376435H", "-PT376435H"],
            ["minutes", "PT376435H23M", "-PT376435H23M"],
            ["seconds", "PT376435H23M8S", "-PT376435H23M8S"],
            ["milliseconds", "PT376435H23M8.149S", "-PT376435H23M8.149S"],
            ["microseconds", "PT376435H23M8.14853S", "-PT376435H23M8.14853S"],
            ["nanoseconds", "PT376435H23M8.1485295S", "-PT376435H23M8.1485295S"],
        ];

        const roundingMode = "halfExpand";
        expected.forEach(([smallestUnit, expectedPositive, expectedNegative]) => {
            const untilPositive = earlier.until(later, { largestUnit, smallestUnit, roundingMode });
            expect(untilPositive.toString()).toBe(expectedPositive);

            const untilNegative = later.until(earlier, { largestUnit, smallestUnit, roundingMode });
            expect(untilNegative.toString()).toBe(expectedNegative);
        });
    });

    test("'halfFloor' rounding mode", () => {
        const expected = [
            ["hours", "PT376435H", "-PT376435H"],
            ["minutes", "PT376435H23M", "-PT376435H23M"],
            ["seconds", "PT376435H23M8S", "-PT376435H23M8S"],
            ["milliseconds", "PT376435H23M8.149S", "-PT376435H23M8.149S"],
            ["microseconds", "PT376435H23M8.148529S", "-PT376435H23M8.14853S"],
            ["nanoseconds", "PT376435H23M8.1485295S", "-PT376435H23M8.1485295S"],
        ];

        const roundingMode = "halfFloor";
        expected.forEach(([smallestUnit, expectedPositive, expectedNegative]) => {
            const untilPositive = earlier.until(later, { largestUnit, smallestUnit, roundingMode });
            expect(untilPositive.toString()).toBe(expectedPositive);

            const untilNegative = later.until(earlier, { largestUnit, smallestUnit, roundingMode });
            expect(untilNegative.toString()).toBe(expectedNegative);
        });
    });

    test("'halfTrunc' rounding mode", () => {
        const expected = [
            ["hours", "PT376435H", "-PT376435H"],
            ["minutes", "PT376435H23M", "-PT376435H23M"],
            ["seconds", "PT376435H23M8S", "-PT376435H23M8S"],
            ["milliseconds", "PT376435H23M8.149S", "-PT376435H23M8.149S"],
            ["microseconds", "PT376435H23M8.148529S", "-PT376435H23M8.148529S"],
            ["nanoseconds", "PT376435H23M8.1485295S", "-PT376435H23M8.1485295S"],
        ];

        const roundingMode = "halfTrunc";
        expected.forEach(([smallestUnit, expectedPositive, expectedNegative]) => {
            const untilPositive = earlier.until(later, { largestUnit, smallestUnit, roundingMode });
            expect(untilPositive.toString()).toBe(expectedPositive);

            const untilNegative = later.until(earlier, { largestUnit, smallestUnit, roundingMode });
            expect(untilNegative.toString()).toBe(expectedNegative);
        });
    });
});
