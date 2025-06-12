test("length is 1", () => {
    expect(Array.prototype.forEach).toHaveLength(1);
});

describe("errors", () => {
    test("callback must be a function", () => {
        expect(() => {
            [].forEach(undefined);
        }).toThrowWithMessage(TypeError, "undefined is not a function");
    });
});

describe("normal behavior", () => {
    test("never calls callback with empty array", () => {
        var callbackCalled = 0;
        expect(
            [].forEach(() => {
                callbackCalled++;
            })
        ).toBeUndefined();
        expect(callbackCalled).toBe(0);
    });

    test("calls callback once for every item", () => {
        var callbackCalled = 0;
        expect(
            [1, 2, 3].forEach(() => {
                callbackCalled++;
            })
        ).toBeUndefined();
        expect(callbackCalled).toBe(3);
    });

    test("callback receives value and index", () => {
        var a = [1, 2, 3];
        a.forEach((value, index) => {
            expect(value).toBe(a[index]);
            expect(index).toBe(a[index] - 1);
        });
    });

    test("callback receives array", () => {
        var callbackCalled = 0;
        var a = [1, 2, 3];
        a.forEach((_, __, array) => {
            callbackCalled++;
            expect(a).toEqual(array);
            a.push("test");
        });
        expect(callbackCalled).toBe(3);
        expect(a).toEqual([1, 2, 3, "test", "test", "test"]);
    });

    test("this value can be modified", () => {
        var t = [];
        [1, 2, 3].forEach(function (value) {
            this.push(value);
        }, t);
        expect(t).toEqual([1, 2, 3]);
    });
});
