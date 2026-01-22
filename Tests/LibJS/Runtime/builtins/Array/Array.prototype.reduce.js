test("length is 1", () => {
    expect(Array.prototype.reduce).toHaveLength(1);
});

describe("errors", () => {
    test("callback must be a function", () => {
        expect(() => {
            [].reduce(undefined);
        }).toThrowWithMessage(TypeError, "undefined is not a function");
    });

    test("reduce of empty array with no initial value", () => {
        expect(() => {
            [].reduce((a, x) => x);
        }).toThrowWithMessage(TypeError, "Reduce of empty array with no initial value");
    });

    test("reduce of array with only empty slots and no initial value", () => {
        expect(() => {
            [, ,].reduce((a, x) => x);
        }).toThrowWithMessage(TypeError, "Reduce of empty array with no initial value");
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        [1, 2].reduce(function () {
            expect(this).toBe(globalThis);
        });

        [1, 2].reduce(function () {
            "use strict";
            expect(this).toBeUndefined();
        });

        var callbackCalled = 0;
        var callback = () => {
            callbackCalled++;
            return true;
        };

        expect([1].reduce(callback)).toBe(1);
        expect(callbackCalled).toBe(0);

        expect([, 1].reduce(callback)).toBe(1);
        expect(callbackCalled).toBe(0);

        callbackCalled = 0;
        expect([1, 2, 3].reduce(callback)).toBeTrue();
        expect(callbackCalled).toBe(2);

        callbackCalled = 0;
        expect([, , 1, 2, 3].reduce(callback)).toBeTrue();
        expect(callbackCalled).toBe(2);

        callbackCalled = 0;
        expect([1, , , 10, , 100, , ,].reduce(callback)).toBeTrue();
        expect(callbackCalled).toBe(2);

        var constantlySad = () => ":^(";
        var result = [].reduce(constantlySad, ":^)");
        expect(result).toBe(":^)");

        result = [":^0"].reduce(constantlySad, ":^)");
        expect(result).toBe(":^(");

        result = [":^0"].reduce(constantlySad);
        expect(result).toBe(":^0");

        result = [5, 4, 3, 2, 1].reduce((accum, elem) => accum + elem);
        expect(result).toBe(15);

        result = [1, 2, 3, 4, 5, 6].reduce((accum, elem) => accum + elem, 100);
        expect(result).toBe(121);

        result = [6, 5, 4, 3, 2, 1].reduce((accum, elem) => {
            return accum + elem;
        }, 100);
        expect(result).toBe(121);

        var indices = [];
        result = ["foo", 1, true].reduce((a, v, i) => {
            indices.push(i);
        });
        expect(result).toBeUndefined();
        expect(indices.length).toBe(2);
        expect(indices[0]).toBe(1);
        expect(indices[1]).toBe(2);

        indices = [];
        result = ["foo", 1, true].reduce((a, v, i) => {
            indices.push(i);
        }, "foo");
        expect(result).toBeUndefined();
        expect(indices).toEqual([0, 1, 2]);

        var mutable = { prop: 0 };
        result = ["foo", 1, true].reduce((a, v) => {
            a.prop = v;
            return a;
        }, mutable);
        expect(result).toBe(mutable);
        expect(result.prop).toBeTrue();

        var a1 = [1, 2];
        var a2 = null;
        a1.reduce((a, v, i, t) => {
            a2 = t;
        });
        expect(a1).toBe(a2);
    });
});
