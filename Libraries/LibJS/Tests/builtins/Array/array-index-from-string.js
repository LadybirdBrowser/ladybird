describe("normal behavior", () => {
    var arr = [3, 4, 5];

    test("basic", () => {
        expect(arr["0"]).toBe(3);
        expect(arr["1"]).toBe(4);
        expect(arr["2"]).toBe(5);
    });

    test("above length", () => {
        expect(arr["3"]).toBeUndefined();
    });
});

describe("strings with extra characters", () => {
    var arr = [3, 4, 5];

    test("whitespace", () => {
        expect(arr[" 0"]).toBeUndefined();
        expect(arr["0 "]).toBeUndefined();
        expect(arr["  0"]).toBeUndefined();
        expect(arr["  0  "]).toBeUndefined();
        expect(arr["  3  "]).toBeUndefined();
    });

    test("leading 0", () => {
        expect(arr["00"]).toBeUndefined();
        expect(arr["01"]).toBeUndefined();
        expect(arr["02"]).toBeUndefined();
        expect(arr["03"]).toBeUndefined();
    });

    test("leading +/-", () => {
        ["+", "-"].forEach(op => {
            expect(arr[op + "0"]).toBeUndefined();
            expect(arr[op + "1"]).toBeUndefined();

            expect(arr[op + "-0"]).toBeUndefined();
            expect(arr[op + "+0"]).toBeUndefined();
        });
    });

    test("combined", () => {
        expect(arr["+00"]).toBeUndefined();
        expect(arr[" +0"]).toBeUndefined();
        expect(arr["  +0 "]).toBeUndefined();
        expect(arr["  00 "]).toBeUndefined();
    });
});
