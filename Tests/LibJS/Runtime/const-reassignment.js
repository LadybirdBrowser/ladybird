test("reassignment to const", () => {
    const constantValue = 1;
    expect(() => {
        constantValue = 2;
    }).toThrowWithMessage(TypeError, "Invalid assignment to const variable");
    expect(constantValue).toBe(1);
});

test("reassignment to const in arrow function with initialization inside", () => {
    expect(() => {
        (() => {
            const constantValue = 1;
            constantValue = 2;
        })();
    }).toThrowWithMessage(TypeError, "Invalid assignment to const variable");
});

test("reassignment to const in arrow function with initialization outside", () => {
    const constantValue = 1;
    expect(() => {
        (() => {
            constantValue = 2;
        })();
    }).toThrowWithMessage(TypeError, "Invalid assignment to const variable");
    expect(constantValue).toBe(1);
});

test("reassignment to const in next expression of for loop", () => {
    expect(() => {
        for (const i = 0; i < 2; i++) {}
    }).toThrowWithMessage(TypeError, "Invalid assignment to const variable");
});

test("reassignment to const in try with finally", () => {
    let finallyExecuted = false;
    expect(() => {
        (() => {
            const constantValue = 1;
            try {
                constantValue = 2;
            } finally {
                finallyExecuted = true;
            }
        })();
    }).toThrowWithMessage(TypeError, "Invalid assignment to const variable");
    expect(finallyExecuted).toBeTrue();
});

test("reassignment to const in try with catch", () => {
    let catchExecuted = false;
    const constantValue = 1;
    try {
        constantValue = 2;
    } catch {
        catchExecuted = true;
    }
    expect(catchExecuted).toBeTrue();
    expect(constantValue).toBe(1);
});

test("reassignment to const should run valueOf", () => {
    let valueOfExecuted = false;
    expect(() => {
        const constantValue = {
            valueOf() {
                valueOfExecuted = true;
                return 1;
            },
        };
        constantValue++;
    }).toThrowWithMessage(TypeError, "Invalid assignment to const variable");
    expect(valueOfExecuted).toBeTrue();
});

test("const creation in inner scope", () => {
    const constantValue = 1;
    do {
        const constantValue = 2;
        expect(constantValue).toBe(2);
    } while (false);
    expect(constantValue).toBe(1);
});
