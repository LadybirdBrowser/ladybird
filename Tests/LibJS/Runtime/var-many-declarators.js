test("var statement with many declarators does not hang", () => {
    const count = 50000;
    const names = Array.from({ length: count }, (_, i) => `v${i}`);
    const source = `var ${names.join(",")};`;
    const start = Date.now();
    eval(source);
    expect(Date.now() - start).toBeLessThan(5000);
});
