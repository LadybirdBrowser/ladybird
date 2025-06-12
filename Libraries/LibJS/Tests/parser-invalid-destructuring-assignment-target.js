test("Assigning to an invalid destructuring assignment target should fail immediately", () => {
    expect(() => {
        eval("[[function=a{1,}=");
    }).toThrow(SyntaxError);
});
