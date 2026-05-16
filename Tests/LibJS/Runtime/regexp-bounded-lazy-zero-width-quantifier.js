test("bounded lazy zero-width group requires progress at optional boundary", () => {
    expect(/(?:a*?){2,3}/.exec("a")[0]).toBe("a");
    expect(/(?:a*?){2,3}/.exec("aa")[0]).toBe("a");
    expect(/(?:a*?){2,3}/.exec("aaa")[0]).toBe("a");
    expect(/(?:a*?){2,3}/.exec("")[0]).toBe("");
    expect(/(?:a*?){2,3}/.exec("b")[0]).toBe("");
});

test("bounded lazy zero-width group with min=1", () => {
    expect(/(?:a*?){1,2}/.exec("a")[0]).toBe("a");
    expect(/(?:a*?){1,2}/.exec("")[0]).toBe("");
});

test("bounded lazy zero-width group with unbounded max", () => {
    expect(/(?:a*?){2,}/.exec("a")[0]).toBe("a");
    expect(/(?:a*?){2,}/.exec("aa")[0]).toBe("aa");
    expect(/(?:a*?){2,}/.exec("")[0]).toBe("");
});
