test("reassignment to const", () => {
    const constantValue = 1;
    expect(() => {
        constantValue = 2;
    }).toThrowWithMessage(TypeError, "Invalid assignment to const variable");
    expect(constantValue).toBe(1);
});

test("const creation in inner scope", () => {
    const constantValue = 1;
    do {
        const constantValue = 2;
        expect(constantValue).toBe(2);
    } while (false);
    expect(constantValue).toBe(1);
});
