describe("Array.prototype.sort", () => {
    test("basic functionality", () => {
        expect(Array.prototype.sort).toHaveLength(1);

        var arr = ["c", "b", "d", "a"];
        expect(arr.sort()).toEqual(arr);
        expect(arr).toEqual(["a", "b", "c", "d"]);

        arr = ["aa", "a"];
        expect(arr.sort()).toEqual(arr);
        expect(arr).toEqual(["a", "aa"]);

        arr = [1, 0];
        expect(arr.sort()).toBe(arr); // should be exactly same object
        expect(arr).toEqual([0, 1]);

        // numbers are sorted as strings
        arr = [205, -123, 22, 200, 3, -20, -2, -1, 25, 2, 0, 1];
        expect(arr.sort()).toEqual([-1, -123, -2, -20, 0, 1, 2, 200, 205, 22, 25, 3]);

        // mix of data, including empty slots and undefined
        arr = ["2", Infinity, null, null, , undefined, 5, , undefined, null, 54, "5"];
        expect(arr.sort()).toEqual([
            "2",
            5,
            "5",
            54,
            Infinity,
            null,
            null,
            null,
            undefined,
            undefined,
            ,
            ,
        ]);
        expect(arr.length).toEqual(12);

        // undefined compare function
        arr = ["2", Infinity, null, null, , undefined, 5n, , undefined, null, 54, "5"];
        expect(arr.sort(undefined)).toEqual([
            "2",
            5n,
            "5",
            54,
            Infinity,
            null,
            null,
            null,
            undefined,
            undefined,
            ,
            ,
        ]);
        expect(arr.length).toEqual(12);

        // numeric data with compare function to sort numerically
        arr = [50, 500, 5, Infinity, -Infinity, 0, 10, -10, 1, -1, 5, 0, 15, Infinity];
        expect(arr.sort((a, b) => a - b)).toEqual([
            -Infinity,
            -10,
            -1,
            0,
            0,
            1,
            5,
            5,
            10,
            15,
            50,
            500,
            Infinity,
            Infinity,
        ]);
        expect(arr.length).toEqual(14);

        // numeric data with compare function to sort reverse numerically
        arr = [50, 500, 5, Infinity, -Infinity, 0, 10, -10, 1, -1, 5, 0, 15, Infinity];
        expect(arr.sort((a, b) => b - a)).toEqual([
            Infinity,
            Infinity,
            500,
            50,
            15,
            10,
            5,
            5,
            1,
            0,
            0,
            -1,
            -10,
            -Infinity,
        ]);

        // small/edge cases
        expect([].sort()).toEqual([]);
        expect([5].sort()).toEqual([5]);
        expect([5, 5].sort()).toEqual([5, 5]);
        expect([undefined].sort()).toEqual([undefined]);
        expect([undefined, undefined].sort()).toEqual([undefined, undefined]);
        expect([,].sort()).toEqual([,]);
        expect([, ,].sort()).toEqual([, ,]);
        expect([5, ,].sort()).toEqual([5, ,]);
        expect([, , 5].sort()).toEqual([5, , ,]);

        // sorting should be stable
        arr = [
            { sorted_key: 2, other_property: 1 },
            { sorted_key: 2, other_property: 2 },
            { sorted_key: 1, other_property: 3 },
        ];
        arr.sort((a, b) => a.sorted_key - b.sorted_key);
        expect(arr[1].other_property == 1);
        expect(arr[2].other_property == 2);
    });

    test("that it makes no unnecessary calls to compare function", () => {
        expectNoCallCompareFunction = function (a, b) {
            expect().fail();
        };

        expect([].sort(expectNoCallCompareFunction)).toEqual([]);
        expect([1].sort(expectNoCallCompareFunction)).toEqual([1]);
        expect([1, undefined].sort(expectNoCallCompareFunction)).toEqual([1, undefined]);
        expect([undefined, undefined].sort(expectNoCallCompareFunction)).toEqual([
            undefined,
            undefined,
        ]);
        expect([, , 1, ,].sort(expectNoCallCompareFunction)).toEqual([1, , , ,]);
        expect([undefined, , 1, , undefined, ,].sort(expectNoCallCompareFunction)).toEqual([
            1,
            undefined,
            undefined,
            ,
            ,
            ,
        ]);
    });

    test("that it works on non-arrays", () => {
        var obj = { length: 0 };
        expect(Array.prototype.sort.call(obj)).toBe(obj);
        expect(obj).toEqual({ length: 0 });

        obj = { 0: 1, length: 0 };
        expect(Array.prototype.sort.call(obj, undefined)).toBe(obj);
        expect(obj).toEqual({ 0: 1, length: 0 });

        obj = { 0: 3, 1: 2, 2: 1, 3: 0, length: 2 };
        expect(Array.prototype.sort.call(obj)).toBe(obj);
        expect(obj).toEqual({ 0: 2, 1: 3, 2: 1, 3: 0, length: 2 });

        obj = { 0: 3, 1: 2, 2: 1, a: "b", hello: "friends!", length: 2 };
        expect(Array.prototype.sort.call(obj)).toBe(obj);
        expect(obj).toEqual({ 0: 2, 1: 3, 2: 1, a: "b", hello: "friends!", length: 2 });

        obj = { 0: 2, 1: 3, 2: 1, a: "b", hello: "friends!", length: 2 };
        expect(
            Array.prototype.sort.call(obj, (a, b) => {
                expect(a == 2 || a == 3).toBeTrue();
                expect(b == 2 || b == 3).toBeTrue();
                return b - a;
            })
        ).toBe(obj);
        expect(obj).toEqual({ 0: 3, 1: 2, 2: 1, a: "b", hello: "friends!", length: 2 });
    });

    test("that it handles abrupt completions correctly", () => {
        class TestError extends Error {
            constructor() {
                super();
                this.name = "TestError";
            }
        }

        arr = [1, 2, 3];
        expect(() =>
            arr.sort((a, b) => {
                throw new TestError();
            })
        ).toThrow(TestError);

        class DangerousToString {
            toString() {
                throw new TestError();
            }
        }
        arr = [new DangerousToString(), new DangerousToString()];
        expect(() => arr.sort()).toThrow(TestError);
    });

    test("that it does not use deleteProperty unnecessarily", () => {
        var obj = new Proxy(
            { 0: 5, 1: 4, 2: 3, length: 3 },
            {
                deleteProperty: function (target, property) {
                    expect().fail();
                },
            }
        );
        Array.prototype.sort.call(obj);
    });
});
