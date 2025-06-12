test("basic functionality", () => {
    let a = [];
    for (let i = 0; i < 3; ++i) {
        a.push(i);
    }
    expect(a).toEqual([0, 1, 2]);
});

test("only condition", () => {
    let a = [];
    for (; a.length < 3; ) {
        a.push("x");
    }
    expect(a).toEqual(["x", "x", "x"]);
});
