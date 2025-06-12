test("length is 1", () => {
    expect(Array.prototype.some).toHaveLength(1);
});

describe("errors", () => {
    test("callback must be a function", () => {
        expect(() => {
            [].some(undefined);
        }).toThrowWithMessage(TypeError, "undefined is not a function");
    });
});

test("basic functionality", () => {
    var array = ["hello", "friends", 1, 2, false, -42, { name: "serenityos" }];

    expect(array.some(value => value === "hello")).toBeTrue();
    expect(array.some(value => value === "serenity")).toBeFalse();
    expect(array.some((value, index, arr) => index === 1)).toBeTrue();
    expect(array.some(value => value == "1")).toBeTrue();
    expect(array.some(value => value === 1)).toBeTrue();
    expect(array.some(value => value === 13)).toBeFalse();
    expect(array.some(value => typeof value !== "string")).toBeTrue();
    expect(array.some(value => typeof value === "boolean")).toBeTrue();
    expect(array.some(value => value > 1)).toBeTrue();
    expect(array.some(value => value > 1 && value < 3)).toBeTrue();
    expect(array.some(value => value > 100)).toBeFalse();
    expect(array.some(value => value < 0)).toBeTrue();
    expect(array.some(value => array.pop())).toBeTrue();
    expect(["", "hello", "friends", "serenity"].some(value => value.length === 0)).toBeTrue();
    expect([].some(value => value === 1)).toBeFalse();
});
