test("if statement consequent/alternate expression returns empty completion", () => {
    expect(eval("1; if (true) {}")).toBeUndefined();
    expect(eval("1; if (false) {} else {}")).toBeUndefined();
});
