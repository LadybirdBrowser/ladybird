test("length is 1", () => {
    expect(Array.prototype.unshift).toHaveLength(1);
});

describe("normal behavior", () => {
    test("no argument", () => {
        var a = ["hello"];
        expect(a.unshift()).toBe(1);
        expect(a).toEqual(["hello"]);
    });

    test("single argument", () => {
        var a = ["hello"];
        expect(a.unshift("friends")).toBe(2);
        expect(a).toEqual(["friends", "hello"]);
    });

    test("multiple arguments", () => {
        var a = ["friends", "hello"];
        expect(a.unshift(1, 2, 3)).toBe(5);
        expect(a).toEqual([1, 2, 3, "friends", "hello"]);
    });
});
