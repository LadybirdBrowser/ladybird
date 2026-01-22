test("length is 1", () => {
    expect(Array.prototype.every).toHaveLength(1);
});

describe("errors", () => {
    test("callback must be a function", () => {
        expect(() => {
            [].every(undefined);
        }).toThrowWithMessage(TypeError, "undefined is not a function");
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        var arrayOne = ["serenity", { test: "serenity" }];
        var arrayTwo = [true, false, 1, 2, 3, "3"];

        expect(arrayOne.every(value => value === "hello")).toBeFalse();
        expect(arrayOne.every(value => value === "serenity")).toBeFalse();
        expect(arrayOne.every((value, index, arr) => index < 2)).toBeTrue();
        expect(arrayOne.every(value => typeof value === "string")).toBeFalse();
        expect(arrayOne.every(value => arrayOne.pop())).toBeTrue();

        expect(arrayTwo.every((value, index, arr) => index > 0)).toBeFalse();
        expect(arrayTwo.every((value, index, arr) => index >= 0)).toBeTrue();
        expect(arrayTwo.every(value => typeof value !== "string")).toBeFalse();
        expect(arrayTwo.every(value => typeof value === "number")).toBeFalse();
        expect(arrayTwo.every(value => value > 0)).toBeFalse();
        expect(arrayTwo.every(value => value >= 0 && value < 4)).toBeTrue();
        expect(arrayTwo.every(value => arrayTwo.pop())).toBeTrue();

        expect(["", "hello", "friends", "serenity"].every(value => value.length >= 0)).toBeTrue();
    });

    test("empty array", () => {
        expect([].every(value => value === 1)).toBeTrue();
    });

    test("elements past the initial array size are ignored", () => {
        var array = [1, 2, 3, 4, 5];

        expect(
            array.every((value, index, arr) => {
                arr.push(6);
                return value <= 5;
            })
        ).toBeTrue();
    });
});
