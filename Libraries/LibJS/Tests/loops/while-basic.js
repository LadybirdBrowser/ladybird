test("basic functionality", () => {
    let number = 0;
    while (number < 9) {
        number++;
    }
    expect(number).toBe(9);
});

test("no braces", () => {
    let number = 0;
    while (number < 3) number++;
    expect(number).toBe(3);
});

test("does not loop when initially false", () => {
    while (false) {
        expect().fail();
    }
});

test("exception in test expression", () => {
    expect(() => {
        while (foo);
    }).toThrow(ReferenceError);
});
