test("greedy \\w and \\W do not apply unicode ignore-case behavior without /u", () => {
    expect(/\w/i.exec("ſ")).toBeNull();
    expect(/\w+/i.exec("ſ")).toBeNull();

    expect(/\W+/i.exec("a😀ſZ")[0]).toBe("😀ſ");
});
