test("bounded quantified non-capturing groups still match", () => {
    expect(/(?:ab|c){0,32}/.exec("ab")).not.toBeNull();
    expect(/(?:ab|c){0,32}/.exec("ab")[0]).toBe("ab");

    expect(/(?:ab|c){0,32}/.exec("c")).not.toBeNull();
    expect(/(?:ab|c){0,32}/.exec("c")[0]).toBe("c");

    expect(/(?:ab|c){0,32}d/.exec("abd")).not.toBeNull();
    expect(/(?:ab|c){0,32}d/.exec("abd")[0]).toBe("abd");
});

test("single-quoted token regex still matches bounded group alternatives", () => {
    let regex = /'(?:\\(?:\r\n|[\s\S])|[^'\\\r\n]){0,32}'/g;
    let input = "x 'Core' y 'Init()'";

    expect(() => input.match(regex)).not.toThrow();
    expect(input.match(regex)).toEqual(["'Core'", "'Init()'"]);
});

test("single-quoted token regex handles a quoted token followed by trailing context", () => {
    let regex = /'(?:\\(?:\r\n|[\s\S])|[^'\\\r\n]){0,32}'/g;
    let input = "'Core'. Such structs must be initialized by";

    expect(() => input.match(regex)).not.toThrow();
    expect(input.match(regex)).toEqual(["'Core'"]);
});

test("bounded quantifier with non-zero min still matches", () => {
    expect(/(?:ab|c){2,10}/.exec("ababc")[0]).toBe("ababc");
    expect(/(?:ab|c){2,11}/.exec("ababc")[0]).toBe("ababc");
    expect(/(a|b){2,10}/.exec("ab")[0]).toBe("ab");
});

test("bounded quantifier on a zero-width group is not compiled via counted loop", () => {
    expect(() => /(?:a*){0,10}b/.exec("b")).not.toThrow();
    expect(/(?:a*){0,10}b/.exec("b")[0]).toBe("b");
});
