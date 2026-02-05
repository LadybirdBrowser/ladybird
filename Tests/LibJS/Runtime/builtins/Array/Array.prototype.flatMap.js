test("length is 1", () => {
    expect(Array.prototype.flatMap).toHaveLength(1);
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        function identity(i) {
            return i;
        }

        var array1 = [1, 2, [3, 4]];
        var array2 = [1, 2, [3, 4, [5, 6]]];
        expect(array1.flatMap(identity)).toEqual([1, 2, 3, 4]);
        // only goes to depth 1
        expect(array2.flatMap(identity)).toEqual([1, 2, 3, 4, [5, 6]]);
    });

    test("flattens return values", () => {
        function double(i) {
            return [i, 2 * i];
        }

        var array1 = [1, 2];
        var array2 = [1, [3]];
        expect(array1.flatMap(double)).toEqual([1, 2, 2, 4]);

        // looks weird but it is correct
        expect(array2.flatMap(double)).toEqual([1, 2, [3], 6]);
    });

    test("binds this value", () => {
        let this_ = undefined;
        function callable() {
            this_ = this;
        }
        const this_arg = { "yak?": "always" };
        [0].flatMap(callable, this_arg);
        expect(this_).toEqual(this_arg);
    });

    test("gives secondary arguments", () => {
        const found_values = [];
        const found_indices = [];
        const found_array_values = [];
        const found_this_values = [];
        function callable(val, index, obj) {
            found_values.push(val);
            found_indices.push(index);
            found_array_values.push(obj);
            found_this_values.push(this);
        }
        const this_arg = { "yak?": "always" };
        const array = ["a", "b", "c"];
        array.flatMap(callable, this_arg);

        expect(found_values).toEqual(["a", "b", "c"]);
        expect(found_indices).toEqual([0, 1, 2]);
        expect(found_array_values).toEqual([array, array, array]);
        expect(found_this_values).toEqual([this_arg, this_arg, this_arg]);
    });

    test("empty array means no calls", () => {
        let called = false;
        function callable() {
            called = true;
            throw "Should not be called";
        }
        [].flatMap(callable);
        expect(called).toBeFalse();
    });
});

test("is unscopable", () => {
    expect(Array.prototype[Symbol.unscopables].flatMap).toBeTrue();
    const array = [];
    with (array) {
        expect(() => {
            flatMap;
        }).toThrowWithMessage(ReferenceError, "'flatMap' is not defined");
    }
});
