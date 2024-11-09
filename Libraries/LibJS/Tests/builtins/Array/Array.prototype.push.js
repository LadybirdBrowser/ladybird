test("length is 1", () => {
    expect(Array.prototype.push).toHaveLength(1);
});

describe("normal behavior", () => {
    test("no argument", () => {
        var a = ["hello"];
        expect(a.push()).toBe(1);
        expect(a).toEqual(["hello"]);
    });

    test("single argument", () => {
        var a = ["hello"];
        expect(a.push("friends")).toBe(2);
        expect(a).toEqual(["hello", "friends"]);
    });

    test("multiple arguments", () => {
        var a = ["hello", "friends"];
        expect(a.push(1, 2, 3)).toBe(5);
        expect(a).toEqual(["hello", "friends", 1, 2, 3]);
    });
});
