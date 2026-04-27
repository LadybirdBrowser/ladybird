test("length is 1", () => {
    expect(Array.prototype.indexOf).toHaveLength(1);
});

test("basic functionality", () => {
    var array = ["hello", "friends", 1, 2, false];

    expect(array.indexOf("hello")).toBe(0);
    expect(array.indexOf("friends")).toBe(1);
    expect(array.indexOf(false)).toBe(4);
    expect(array.indexOf(false, 2)).toBe(4);
    expect(array.indexOf(false, -2)).toBe(4);
    expect(array.indexOf(1)).toBe(2);
    expect(array.indexOf(1, 1000)).toBe(-1);
    expect(array.indexOf(1, -1000)).toBe(2);
    expect(array.indexOf("serenity")).toBe(-1);
    expect(array.indexOf(false, -1)).toBe(4);
    expect(array.indexOf(2, -1)).toBe(-1);
    expect(array.indexOf(2, -2)).toBe(3);
    expect([].indexOf("serenity")).toBe(-1);
    expect([].indexOf("serenity", 10)).toBe(-1);
    expect([].indexOf("serenity", -10)).toBe(-1);
    expect([].indexOf()).toBe(-1);
    expect([undefined].indexOf()).toBe(0);
});

test("fromIndex side effects can mutate array length", () => {
    var array = ["hello", "friends"];
    var fromIndex = {
        valueOf() {
            array.pop();
            return 0;
        },
    };

    expect(array.indexOf("friends", fromIndex)).toBe(-1);
});

test("fromIndex side effects can grow array past captured length", () => {
    var array = ["hello", "friends"];
    var fromIndex = {
        valueOf() {
            array.push("serenity");
            return 0;
        },
    };

    expect(array.indexOf("serenity", fromIndex)).toBe(-1);
});

test("fromIndex side effects can mutate packed array elements", () => {
    var array = ["hello", "friends"];
    var fromIndex = {
        valueOf() {
            array[1] = "serenity";
            return 0;
        },
    };

    expect(array.indexOf("serenity", fromIndex)).toBe(1);
});

test("fromIndex side effects can make prototype indexed properties observable", () => {
    var array = ["hello", "friends"];
    var fromIndex = {
        valueOf() {
            delete array[1];
            Array.prototype[1] = "serenity";
            return 0;
        },
    };

    try {
        expect(array.indexOf("serenity", fromIndex)).toBe(1);
    } finally {
        delete Array.prototype[1];
    }
});
