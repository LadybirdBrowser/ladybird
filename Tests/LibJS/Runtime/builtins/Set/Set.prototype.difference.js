describe("errors", () => {
    test("called with negative size", () => {
        expect(() => {
            new Set().difference({ size: -1 });
        }).toThrowWithMessage(RangeError, "size must not be negative");
    });
});

test("basic functionality", () => {
    expect(Set.prototype.difference).toHaveLength(1);

    const set1 = new Set(["a", "b", "c"]);
    const set2 = new Set(["b", "c", "d", "e"]);
    const difference1to2 = set1.difference(set2);
    expect(difference1to2).toHaveSize(1);
    expect(difference1to2.has("a")).toBeTrue();
    const difference2to1 = set2.difference(set1);
    expect(difference2to1).toHaveSize(2);
    ["d", "e"].forEach(value => expect(difference2to1.has(value)).toBeTrue());
});

test("receiver mutations during other.has do not affect visited keys", () => {
    const set = new Set([1, 2, 3, 4]);
    const visited = [];

    const other = {
        size: 100,
        has(value) {
            visited.push(value);

            if (visited.length === 1) {
                set.clear();
                set.add(11);
                set.add(22);
            }

            return true;
        },
        keys() {
            throw new Error("unexpected keys call");
        },
    };

    const difference = set.difference(other);
    expect(difference).toHaveSize(0);
    expect(visited).toEqual([1, 2, 3, 4]);
    expect([...set]).toEqual([11, 22]);
});
