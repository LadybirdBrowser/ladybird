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

test("update expression on const keeps its value", () => {
    function localConstant() {
        const value = 1;
        try {
            ++value;
        } catch {}
        try {
            value--;
        } catch {}
        return value;
    }
    expect(localConstant()).toBe(1);
});

test("a single update expression on const keeps its value", () => {
    function prefix() {
        const value = 1;
        try {
            ++value;
        } catch {}
        return value;
    }
    function postfix() {
        const value = "1";
        try {
            value--;
        } catch {}
        return value;
    }
    expect(prefix()).toBe(1);
    expect(postfix()).toBe("1");
});

test("const creation in inner scope", () => {
    const constantValue = 1;
    do {
        const constantValue = 2;
        expect(constantValue).toBe(2);
    } while (false);
    expect(constantValue).toBe(1);
});

test("reassignment to destructured const in function", () => {
    expect(() => {
        (() => {
            const { value } = { value: 1 };
            value = 2;
        })();
    }).toThrowWithMessage(TypeError, "Invalid assignment to const variable");

    expect(() => {
        (() => {
            const [value] = [1];
            [value] = [2];
        })();
    }).toThrowWithMessage(TypeError, "Invalid assignment to const variable");
});

test("updates of a const run valueOf and leave the value as it is", () => {
    let valueOfCount = 0;
    const object = {
        valueOf() {
            ++valueOfCount;
            return 1;
        },
    };
    function postfix(value) {
        const constant = value;
        constant++;
    }
    function prefix(value) {
        const constant = value;
        --constant;
    }
    expect(() => postfix(object)).toThrowWithMessage(TypeError, "Invalid assignment to const variable");
    expect(() => prefix(object)).toThrowWithMessage(TypeError, "Invalid assignment to const variable");
    expect(valueOfCount).toBe(2);

    function keepsValue() {
        const constant = Math.abs(1);
        try {
            constant++;
        } catch {}
        try {
            --constant;
        } catch {}
        return constant;
    }
    expect(keepsValue()).toBe(1);
});
