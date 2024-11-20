test("basic functionality", () => {
    expect(Date.parse("2020")).toBe(1577836800000);
    expect(Date.parse("2000-11")).toBe(973036800000);
    expect(Date.parse("1980-06-30")).toBe(331171200000);
    expect(Date.parse("1970-06-30T13:30Z")).toBe(15600600000);
    expect(Date.parse("1970-01-01T00:00:59Z")).toBe(59000);
    expect(Date.parse("1970-01-01T00:00:00.999Z")).toBe(999);
    expect(Date.parse("2020T13:14+15:16")).toBe(1577829480000);
    expect(Date.parse("2020T13:14-15:16")).toBe(1577939400000);
    expect(Date.parse("2020T23:59Z")).toBe(1577923140000);

    expect(Date.parse("+020000")).toBe(568971820800000);
    expect(Date.parse("-000001")).toBe(-62198755200000);
    expect(Date.parse("+020000-01")).toBe(568971820800000);
    expect(Date.parse("+020000-01T00:00:00.000Z")).toBe(568971820800000);

    const originalTimeZone = setTimeZone("UTC");

    expect(Date.parse("1980-5-30")).toBe(328492800000);

    setTimeZone("America/Chicago");
    expect(Date.parse("Jan 01 1970 GMT")).toBe(0);
    expect(Date.parse("Wed Apr 17 23:08:53 2019 +0000")).toBe(1555542533000);
    expect(Date.parse("2021-07-01 03:00Z")).toBe(1625108400000);
    expect(Date.parse("2024-01-08 9:00Z")).toBe(1704704400000);
    expect(Date.parse("Wed, 17 Jan 2024 11:36:34 +0000")).toBe(1705491394000);
    expect(Date.parse("Sun Jan 21 2024 21:11:31 GMT 0100 (Central European Standard Time)")).toBe(
        1705867891000
    );
    expect(Date.parse("05 Jul 2024 00:00")).toBe(1720155600000);
    expect(Date.parse("05 Jul 2024")).toBe(1720155600000);
    expect(Date.parse("05 July 2024")).toBe(1720155600000);
    expect(Date.parse("05 July 2024 00:00")).toBe(1720155600000);
    expect(Date.parse("2024-07-05 00:00:00 GMT-0200")).toBe(1720144800000);
    expect(Date.parse("2024-01-15 00:00:01")).toBe(1705298401000);
    expect(Date.parse("Tue Nov 07 2023 10:05:55  UTC")).toBe(1699351555000);
    expect(Date.parse("Wed Apr 17 23:08:53 2019")).toBe(1555560533000);
    expect(Date.parse("Wed Apr 17 2019 23:08:53")).toBe(1555560533000);
    expect(Date.parse("2024-01-26T22:10:11.306+0000")).toBe(1706307011000); // FIXME: support sub-second precision
    expect(Date.parse("1/27/2024, 9:28:30 AM")).toBe(1706369310000);
    expect(Date.parse("01 February 2013")).toBe(1359698400000);
    expect(Date.parse("Tuesday, October 29, 2024, 18:00 UTC")).toBe(1730224800000);
    expect(Date.parse("November 19 2024 00:00:00 +0900")).toBe(1731942000000);
    expect(Date.parse("Wed Nov 20 2024")).toBe(1732082400000);

    // FIXME: Create a scoped time zone helper when bytecode supports the `using` declaration.
    setTimeZone(originalTimeZone);

    expect(Date.parse(2020)).toBe(1577836800000);

    expect(Date.parse("+1980")).toBe(NaN);
    expect(Date.parse("1980-")).toBe(NaN);
    expect(Date.parse("1980-05-")).toBe(NaN);
    expect(Date.parse("1980-05-00T")).toBe(NaN);
    expect(Date.parse("1980-05-00T15:15:")).toBe(NaN);
    expect(Date.parse("1980-05-00T15:15:15.")).toBe(NaN);
    expect(Date.parse("1980-05-30T13")).toBe(NaN);
    expect(Date.parse("1980-05-30T13:4")).toBe(NaN);
    expect(Date.parse("1980-05-30T13:40+")).toBe(NaN);
    expect(Date.parse("1980-05-30T13:40+1")).toBe(NaN);
    expect(Date.parse("1980-05-30T13:40+1:10")).toBe(NaN);
    expect(Date.parse("1970-06-30T13:30Zoo")).toBe(NaN);
    expect(Date.parse("2020T13:30.40:")).toBe(NaN);
    expect(Date.parse("-000000")).toBe(NaN);
});

test("time clip", () => {
    expect(Date.parse("+999999")).toBeNaN();
    expect(Date.parse("-999999")).toBeNaN();
});

test("extra micro seconds extension", () => {
    expect(Date.parse("2021-04-30T15:19:02.937+00:00")).toBe(1619795942937);
    expect(Date.parse("2021-04-30T15:19:02.9370+00:00")).toBe(1619795942937);
    expect(Date.parse("2021-04-30T15:19:02.93700+00:00")).toBe(1619795942937);
    expect(Date.parse("2021-04-30T15:19:02.937000+00:00")).toBe(1619795942937);

    expect(Date.parse("2021-04-30T15:19:02.93+00:00")).toBe(1619795942930);
    expect(Date.parse("2021-04-30T15:19:02.9+00:00")).toBe(1619795942900);

    // These values are just checked against NaN since they don't have a specified timezone.
    expect(Date.parse("2021-04-30T15:19:02.93")).not.toBe(NaN);
    expect(Date.parse("2021-04-30T15:19:02.9")).not.toBe(NaN);

    expect(Date.parse("2021-04-30T15:19:02.+00:00")).toBe(NaN);
    expect(Date.parse("2021-04-30T15:19:02.")).toBe(NaN);
    expect(Date.parse("2021-04-30T15:19:02.a")).toBe(NaN);
    expect(Date.parse("2021-04-30T15:19:02.000a")).toBe(NaN);

    expect(Date.parse("2021-04-30T15:19:02.937001+00:00")).toBe(1619795942937);
    expect(Date.parse("2021-04-30T15:19:02.937999+00:00")).toBe(1619795942937);

    expect(Date.parse("2021-06-26T07:24:40.007000+00:00")).toBe(1624692280007);
    expect(Date.parse("2021-06-26T07:24:40.0079999999999999999+00:00")).toBe(1624692280007);
    expect(Date.parse("2021-04-15T18:47:25.606000+00:00")).toBe(1618512445606);
});

test("extra date extension", () => {
    function expectStringToGiveDate(input, fullYear, month, dayInMonth) {
        // Since the timezone is not specified we just say it has to equal the date parts.
        const date = new Date(Date.parse(input));
        expect(date.getFullYear()).toBe(fullYear);
        expect(date.getMonth() + 1).toBe(month);
        expect(date.getDate()).toBe(dayInMonth);
    }

    expectStringToGiveDate("01/30/2021", 2021, 1, 30);
    expectStringToGiveDate("10/1/2021", 2021, 10, 1);
    expectStringToGiveDate("7/07/1977", 1977, 7, 7);
    expectStringToGiveDate("2/27/3058", 3058, 2, 27);
});

test("mm/dd/yy hh:mm timezone-offset extension", () => {
    // Examples from Discord's JavaScript for Christmas 2022.
    expect(Date.parse("12/05/2022 10:00 -0800")).toBe(1670263200000);
    expect(Date.parse("01/03/2023 10:00 -0800")).toBe(1672768800000);
});

test("yy{/,-}mm{/,-}dd hh:mm extension", () => {
    function expectStringToGiveDate(input, fullYear, month, dayInMonth, hours, minutes) {
        // Since the timezone is not specified we just say it has to equal the date parts.
        const date = new Date(Date.parse(input));
        expect(date.getFullYear()).toBe(fullYear);
        expect(date.getMonth() + 1).toBe(month);
        expect(date.getDate()).toBe(dayInMonth);
        expect(date.getHours()).toBe(hours);
        expect(date.getMinutes()).toBe(minutes);
    }

    // Example from a UK news website.
    expectStringToGiveDate("2014/11/14 13:05", 2014, 11, 14, 13, 5);
    expectStringToGiveDate("2014-11-14 13:05", 2014, 11, 14, 13, 5);
});

test("Month dd, yy extension", () => {
    function expectStringToGiveDate(input, fullYear, month, dayInMonth) {
        const date = new Date(Date.parse(input));
        expect(date.getFullYear()).toBe(fullYear);
        expect(date.getMonth() + 1).toBe(month);
        expect(date.getDate()).toBe(dayInMonth);
    }

    expectStringToGiveDate("May 15, 2023", 2023, 5, 15);
    expectStringToGiveDate("May 22, 2023", 2023, 5, 22);
    expectStringToGiveDate("May 30, 2023", 2023, 5, 30);
    expectStringToGiveDate("June 5, 2023", 2023, 6, 5);
});

test("Month dd, yy hh:mm:ss extension", () => {
    function expectStringToGiveDate(input, fullYear, month, dayInMonth, hours, minutes, seconds) {
        // Since the timezone is not specified we just say it has to equal the date parts.
        const date = new Date(Date.parse(input));
        expect(date.getFullYear()).toBe(fullYear);
        expect(date.getMonth() + 1).toBe(month);
        expect(date.getDate()).toBe(dayInMonth);
        expect(date.getHours()).toBe(hours);
        expect(date.getMinutes()).toBe(minutes);
        expect(date.getSeconds()).toBe(seconds);
    }

    // Examples from Discord's Birthday JavaScript for May 2023.
    expectStringToGiveDate("May 15, 2023 17:00:00", 2023, 5, 15, 17, 0, 0);
    expectStringToGiveDate("May 22, 2023 17:00:00", 2023, 5, 22, 17, 0, 0);
    expectStringToGiveDate("May 30, 2023 17:00:00", 2023, 5, 30, 17, 0, 0);
    expectStringToGiveDate("June 5, 2023 17:00:00", 2023, 6, 5, 17, 0, 0);
});

test("Date.prototype.toString extension", () => {
    function expectStringToGiveDate(input, fullYear, month, dayInMonth, hours) {
        const date = new Date(Date.parse(input));
        expect(date.getFullYear()).toBe(fullYear);
        expect(date.getMonth() + 1).toBe(month);
        expect(date.getDate()).toBe(dayInMonth);
        expect(date.getHours()).toBe(hours);
    }

    const time1 = "Tue Nov 07 2023 10:00:00 GMT-0500 (Eastern Standard Time)";
    const time2 = "Tue Nov 07 2023 10:00:00 GMT+0100 (Central European Standard Time)";
    const time3 = "Tue Nov 07 2023 10:00:00 GMT+0800 (Australian Western Standard Time)";

    const originalTimeZone = setTimeZone("UTC");
    expectStringToGiveDate(time1, 2023, 11, 7, 15);
    expectStringToGiveDate(time2, 2023, 11, 7, 9);
    expectStringToGiveDate(time3, 2023, 11, 7, 2);

    setTimeZone("America/New_York");
    expectStringToGiveDate(time1, 2023, 11, 7, 10);
    expectStringToGiveDate(time2, 2023, 11, 7, 4);
    expectStringToGiveDate(time3, 2023, 11, 6, 21);

    setTimeZone("Australia/Perth");
    expectStringToGiveDate(time1, 2023, 11, 7, 23);
    expectStringToGiveDate(time2, 2023, 11, 7, 17);
    expectStringToGiveDate(time3, 2023, 11, 7, 10);

    // FIXME: Create a scoped time zone helper when bytecode supports the `using` declaration.
    setTimeZone(originalTimeZone);
});

test("Date.prototype.toUTCString extension", () => {
    function expectStringToGiveDate(input, fullYear, month, dayInMonth, hours) {
        const date = new Date(Date.parse(input));
        expect(date.getFullYear()).toBe(fullYear);
        expect(date.getMonth() + 1).toBe(month);
        expect(date.getDate()).toBe(dayInMonth);
        expect(date.getHours()).toBe(hours);
    }

    const time = "Tue, 07 Nov 2023 15:00:00 GMT";

    const originalTimeZone = setTimeZone("UTC");
    expectStringToGiveDate(time, 2023, 11, 7, 15);

    setTimeZone("America/New_York");
    expectStringToGiveDate(time, 2023, 11, 7, 10);

    setTimeZone("Australia/Perth");
    expectStringToGiveDate(time, 2023, 11, 7, 23);

    // FIXME: Create a scoped time zone helper when bytecode supports the `using` declaration.
    setTimeZone(originalTimeZone);
});

test("Round trip Date.prototype.to*String", () => {
    const epoch = new Date(0);

    expect(Date.parse(epoch.toString())).toBe(epoch.valueOf());
    expect(Date.parse(epoch.toISOString())).toBe(epoch.valueOf());
    expect(Date.parse(epoch.toUTCString())).toBe(epoch.valueOf());
});
