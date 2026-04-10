test("string substring-producing operations work on ropes", () => {
    let rope = "ab" + "cd";

    expect(rope.at(1)).toBe("b");
    expect(rope.charAt(2)).toBe("c");
    expect(rope.slice(1, 3)).toBe("bc");
    expect(rope.substring(1, 3)).toBe("bc");
    expect(rope.substr(1, 2)).toBe("bc");
    expect(rope.split("b")).toEqual(["a", "cd"]);
    expect(rope[2]).toBe("c");
    expect(new String(rope)[2]).toBe("c");
});

test("string substring-producing operations preserve UTF-16 code units on ropes", () => {
    let rope = "😀" + "x";

    expect(rope.at(0)).toBe("\ud83d");
    expect(rope.charAt(1)).toBe("\ude00");
    expect(rope.slice(1, 3)).toBe("\ude00x");
    expect(rope.substring(1, 3)).toBe("\ude00x");
    expect(rope.substr(1, 2)).toBe("\ude00x");
    expect(rope.split("x")).toEqual(["😀", ""]);
    expect(rope[1]).toBe("\ude00");
    expect(new String(rope)[1]).toBe("\ude00");
});
