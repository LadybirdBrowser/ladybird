test("length", () => {
    expect(Map.prototype.values.length).toBe(0);
});

test("basic functionality", () => {
    const original = [
        ["a", 0],
        ["b", 1],
        ["c", 2],
    ];
    const a = new Map(original);
    const it = a.values();
    expect(it.next()).toEqual({ value: 0, done: false });
    expect(it.next()).toEqual({ value: 1, done: false });
    expect(it.next()).toEqual({ value: 2, done: false });
    expect(it.next()).toEqual({ value: undefined, done: true });
    expect(it.next()).toEqual({ value: undefined, done: true });
    expect(it.next()).toEqual({ value: undefined, done: true });
});

describe("empty maps give no values", () => {
    test("always empty", () => {
        const map = new Map();
        const iterator = map.values();

        expect(iterator.next()).toBeIteratorResultDone();
        expect(iterator.next()).toBeIteratorResultDone();
    });

    test("just emptied map", () => {
        const map = new Map([
            [1, 2],
            [3, 4],
        ]);

        const iterator = map.values();

        expect(map.delete(1)).toBeTrue();
        expect(map.delete(3)).toBeTrue();

        expect(iterator.next()).toBeIteratorResultDone();
        expect(iterator.next()).toBeIteratorResultDone();
    });

    test("cleared map", () => {
        const map = new Map([
            [1, 2],
            [3, 4],
        ]);

        const iterator = map.values();

        map.clear();

        expect(iterator.next()).toBeIteratorResultDone();
        expect(iterator.next()).toBeIteratorResultDone();
    });

    test("added and then removed elements", () => {
        const map = new Map([[1, 2]]);

        const iterator = map.values();

        map.set(3, 4);

        map.delete(3);
        map.set(5, 6);
        map.delete(1);
        map.set(1, 4);
        map.delete(5);
        map.delete(1);

        expect(map).toHaveSize(0);

        expect(iterator.next()).toBeIteratorResultDone();
        expect(iterator.next()).toBeIteratorResultDone();
    });
});
