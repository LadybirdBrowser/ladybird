function take(...values) {
    return values;
}

test("each write takes a fresh shared snapshot", () => {
    function repeatedWrites(x) {
        return take(x, x, (x = 2), x, x, (x = 3), x);
    }
    expect(repeatedWrites(1)).toEqual([1, 1, 2, 2, 2, 3, 3]);

    function rawObject() {
        let value = { valueOf: () => 4 };
        const original = value;
        const result = take(value, value, value++);
        return [result[0] === original, result[1] === original, result[2], value];
    }
    expect(rawObject()).toEqual([true, true, 4, 5]);
});

test("shared snapshots preserve callees, receivers, and assignment targets", () => {
    function callee() {
        let fn = function old(a, b, c) {
            return [a === old, b === old, c.name];
        };
        return fn(fn, fn, (fn = function newer() {}));
    }
    expect(callee()).toEqual([true, true, "newer"]);

    function receiver(flag) {
        let base = {
            name: "old",
            method(a, b) {
                return [this.name, a.name, b.name];
            },
        };
        return base.method(base, flag ? (base = { name: "new" }) : base);
    }
    expect(receiver(true)).toEqual(["old", "old", "new"]);
    expect(receiver(false)).toEqual(["old", "old", "old"]);

    function indexedTarget() {
        const object = {};
        let key = "x";
        const result = take(key, (object[key] = key = "y"), key);
        return [result, object.x, Object.hasOwn(object, "y")];
    }
    expect(indexedTarget()).toEqual([["x", "y", "y"], "y", false]);
});

test("shared snapshots survive held registers and distinguish locals from arguments", () => {
    function classKey(flag, x) {
        const result = take(
            x,
            x,
            class {
                [flag ? (x = 2) : (x = 3)]() {}
            },
            x
        );
        return [result[0], result[1], Object.hasOwn(result[2].prototype, flag ? "2" : "3"), result[3]];
    }
    expect(classKey(true, 1)).toEqual([1, 1, true, 2]);
    expect(classKey(false, 1)).toEqual([1, 1, true, 3]);

    function pattern(x) {
        return take(x, x, ([x] = [7]), x);
    }
    expect(pattern(1)).toEqual([1, 1, [7], 7]);

    function distinct(argument, flag) {
        let local = 2;
        return take(argument, local, argument, local, flag ? ((argument = 3), (local = 4)) : null, argument, local);
    }
    expect(distinct(1, true)).toEqual([1, 2, 1, 2, 4, 3, 4]);
    expect(distinct(1, false)).toEqual([1, 2, 1, 2, null, 1, 2]);
});
