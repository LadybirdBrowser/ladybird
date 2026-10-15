test("replaced adders are called", () => {
    const originalAdd = Set.prototype.add;
    const originalSet = Map.prototype.set;
    const added = [];
    Set.prototype.add = function (value) {
        added.push(value);
        return originalAdd.call(this, value);
    };
    Map.prototype.set = function (key, value) {
        added.push([key, value]);
        return originalSet.call(this, key, value);
    };
    try {
        new Set([1, 2]);
        new Map([[3, 4]]);
        expect(added).toEqual([1, 2, [3, 4]]);
    } finally {
        Set.prototype.add = originalAdd;
        Map.prototype.set = originalSet;
    }
});

test("-0 is added as +0", () => {
    const set = new Set([-0]);
    expect(Object.is([...set][0], 0)).toBeTrue();
    const map = new Map([[-0, "zero"]]);
    expect(Object.is([...map.keys()][0], 0)).toBeTrue();
});

test("builtin adders add the values of any iterable", () => {
    function* values() {
        yield 1;
        yield 1;
        yield 2;
    }
    expect([...new Set(values())]).toEqual([1, 2]);
    expect([...new Map(new Set([[1, "a"]]))]).toEqual([[1, "a"]]);
});
