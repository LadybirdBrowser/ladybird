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

test("uses ordinary Set semantics for appended elements", () => {
    var setter_calls = [];
    Object.defineProperty(Array.prototype, 2, {
        set(value) {
            setter_calls.push(value);
        },
        configurable: true,
    });

    try {
        var a = [1, 2];
        expect(a.push(3)).toBe(3);
        expect(setter_calls).toEqual([3]);
        expect(a.hasOwnProperty(2)).toBeFalse();
        expect(a.length).toBe(3);
    } finally {
        delete Array.prototype[2];
    }
});

test("throws when appending to a non-extensible packed array", () => {
    var a = Object.preventExtensions([1, 2]);
    expect(() => {
        a.push(3);
    }).toThrow(TypeError);
    expect(a).toEqual([1, 2]);
});
