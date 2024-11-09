describe("errors", () => {
    test("invalid array length value", () => {
        var a = [1, 2, 3];
        [undefined, "foo", -1, Infinity, -Infinity, NaN].forEach(value => {
            expect(() => {
                a.length = value;
            }).toThrowWithMessage(RangeError, "Invalid array length");
            expect(a).toHaveLength(3);
        });
    });
});

describe("normal behavior", () => {
    test("extend array by setting length", () => {
        var a = [1, 2, 3];
        a.length = 5;
        expect(a).toEqual([1, 2, 3, undefined, undefined]);
    });

    test("truncate array by setting length", () => {
        var a = [1, 2, 3];
        a.length = 2;
        expect(a).toEqual([1, 2]);
        a.length = 0;
        expect(a).toEqual([]);
    });

    test("length value is coerced to number if possible", () => {
        var a = [1, 2, 3];
        a.length = "42";
        expect(a).toHaveLength(42);
        a.length = [];
        expect(a).toHaveLength(0);
        a.length = true;
        expect(a).toHaveLength(1);
    });

    test("setting a huge array length", () => {
        var a = [];
        a.length = 0x80000000;
        expect(a.length).toEqual(0x80000000);

        var b = [];
        b.length = 0x80000001;
        expect(b.length).toEqual(0x80000001);
    });

    test("should not remove non-configurable values", () => {
        var a = [1, undefined, 3];
        Object.defineProperty(a, 1, { configurable: false, value: 2 });
        expect(a.length).toEqual(3);

        expect((a.length = 1)).toEqual(1);
        expect(a.length).toEqual(2);
        expect(a[1]).toEqual(2);
    });
});

describe("behavior when obj has Array prototype", () => {
    function ArrExtend() {}
    ArrExtend.prototype = [10, 11, 12];

    test("Has the properties from prototype", () => {
        var arr = new ArrExtend();
        expect(arr.length).toEqual(3);
        expect(arr[0]).toEqual(10);
        expect(arr[1]).toEqual(11);
        expect(arr[2]).toEqual(12);
    });

    test("Can override length to any value", () => {
        [null, "Hello friends :^)", -6, 0].forEach(value => {
            var arr = new ArrExtend();
            arr.length = value;
            expect(arr.length).toEqual(value);

            // should not wipe high values
            expect(arr[0]).toEqual(10);
            expect(arr[1]).toEqual(11);
            expect(arr[2]).toEqual(12);
        });
    });

    test("Can call array methods", () => {
        var arr = new ArrExtend();
        arr.push(1);
        expect(arr.length).toEqual(4);
        expect(arr[3]).toEqual(1);
    });

    test("If length overwritten uses that value", () => {
        [null, "Hello friends :^)", -6, 0].forEach(value => {
            var arr = new ArrExtend();
            arr.length = value;
            expect(arr.length).toEqual(value);

            arr.push(99);
            expect(arr.length).toEqual(1);
            expect(arr[0]).toEqual(99);

            // should not wipe higher value
            expect(arr[1]).toEqual(11);
            expect(arr[2]).toEqual(12);

            arr.push(100);

            expect(arr.length).toEqual(2);
            expect(arr[1]).toEqual(100);

            arr.length = 0;
            // should not wipe values since we are not an array
            expect(arr[0]).toEqual(99);
            expect(arr[1]).toEqual(100);
            expect(arr[2]).toEqual(12);
        });
    });
});
