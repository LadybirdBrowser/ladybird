test("length is 0", () => {
    expect(Array.prototype.flat).toHaveLength(0);
});

describe("error", () => {
    test("Issue #9317, stack overflow in flatten_into_array from flat call", () => {
        var a = [];
        a[0] = a;
        expect(() => {
            a.flat(3893232121);
        }).toThrowWithMessage(InternalError, "Call stack size limit exceeded");
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        var array1 = [1, 2, [3, 4]];
        var array2 = [1, 2, [3, 4, [5, 6]]];
        var array3 = [1, 2, [3, 4, [5, 6]]];
        expect(array1.flat()).toEqual([1, 2, 3, 4]);
        expect(array2.flat()).toEqual([1, 2, 3, 4, [5, 6]]);
        expect(array3.flat(2)).toEqual([1, 2, 3, 4, 5, 6]);
    });

    test("calls depth as infinity", () => {
        var array1 = [1, 2, [3, 4, [5, 6, [7, 8]]]];
        expect(array1.flat(Infinity)).toEqual([1, 2, 3, 4, 5, 6, 7, 8]);
        expect(array1.flat(-Infinity)).toEqual([1, 2, [3, 4, [5, 6, [7, 8]]]]);
    });

    test("calls depth as undefined", () => {
        var array1 = [1, 2, [3, 4, [5, 6, [7, 8]]]];
        expect(array1.flat(undefined)).toEqual([1, 2, 3, 4, [5, 6, [7, 8]]]);
    });

    test("calls depth as null", () => {
        var array1 = [1, 2, [3, 4, [5, 6, [7, 8]]]];
        expect(array1.flat(null)).toEqual([1, 2, [3, 4, [5, 6, [7, 8]]]]);
        expect(array1.flat(NaN)).toEqual([1, 2, [3, 4, [5, 6, [7, 8]]]]);
    });

    test("calls depth as non integer", () => {
        var array1 = [1, 2, [3, 4, [5, 6, [7, 8]]]];
        expect(array1.flat("depth")).toEqual([1, 2, [3, 4, [5, 6, [7, 8]]]]);
        expect(array1.flat("2")).toEqual([1, 2, 3, 4, 5, 6, [7, 8]]);
        expect(array1.flat(2.1)).toEqual([1, 2, 3, 4, 5, 6, [7, 8]]);
        expect(array1.flat(0.7)).toEqual([1, 2, [3, 4, [5, 6, [7, 8]]]]);
        expect(array1.flat([2])).toEqual([1, 2, 3, 4, 5, 6, [7, 8]]);
        expect(array1.flat([2, 1])).toEqual([1, 2, [3, 4, [5, 6, [7, 8]]]]);
        expect(array1.flat({})).toEqual([1, 2, [3, 4, [5, 6, [7, 8]]]]);
        expect(array1.flat({ depth: 2 })).toEqual([1, 2, [3, 4, [5, 6, [7, 8]]]]);
    });
});

test("is unscopable", () => {
    expect(Array.prototype[Symbol.unscopables].flat).toBeTrue();
    const array = [];
    with (array) {
        expect(() => {
            flat;
        }).toThrowWithMessage(ReferenceError, "'flat' is not defined");
    }
});
