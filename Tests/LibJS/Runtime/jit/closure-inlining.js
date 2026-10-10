// Calls of closures that hot functions create themselves, which the JIT inlines without feedback, as each call of
// the outer function creates a new closure.

const ROUNDS = 300;

test("closures read the variables they capture", () => {
    function addAll(values, offset) {
        let total = 0;
        const add = value => {
            total += value + offset;
        };
        for (const value of values) add(value);
        return total;
    }
    for (let i = 0; i < ROUNDS; ++i) expect(addAll([1, 2], i)).toBe(3 + 2 * i);
    expect(addAll([1.5], 0.25)).toBe(1.75);
    expect(addAll(["a"], "b")).toBe("0ab");
});

test("closures passed to inlined builtins", () => {
    function matching(items, id) {
        return items.filter(item => item.id === id).map(item => ({ id: item.id, title: item.title + "!" }));
    }
    const items = [
        { id: 1, title: "one" },
        { id: 2, title: "two" },
    ];
    for (let i = 0; i < ROUNDS; ++i) {
        const id = 1 + (i % 2);
        expect(matching(items, id)).toEqual([{ id, title: (id === 1 ? "one" : "two") + "!" }]);
    }
    expect(matching([{ id: 1, title: 5 }], 1)).toEqual([{ id: 1, title: "5!" }]);
    expect(matching([{ id: "1" }], 1)).toEqual([]);
});

test("exits, exceptions and stack traces inside inlined closures", () => {
    function run(values, failAt) {
        const seen = [];
        values.forEach(value => {
            if (value === failAt) throw new Error(`failed at ${value} ${new Error().stack.includes("at forEach\n")}`);
            seen.push(value * 2);
        });
        return seen;
    }
    for (let i = 0; i < ROUNDS; ++i) expect(run([1, 2, 3], -1)).toEqual([2, 4, 6]);
    expect(run([1.5, "x", 3], -1)).toEqual([3, NaN, 6]);
    expect(() => run([1, 2, 3], 2)).toThrowWithMessage(Error, "failed at 2 true");
    expect(run([4], -1)).toEqual([8]);
});

test("closures that change what they captured", () => {
    function counter(values) {
        let count = 0;
        const bump = () => ++count;
        values.forEach(bump);
        values.forEach(() => {
            count *= 2;
        });
        return count;
    }
    for (let i = 0; i < ROUNDS; ++i) expect(counter([1, 2])).toBe(8);
    expect(counter([])).toBe(0);
});
