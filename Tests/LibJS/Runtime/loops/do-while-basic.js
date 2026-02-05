test("basic functionality", () => {
    let number = 0;
    do {
        number++;
    } while (number < 9);
    expect(number).toBe(9);
});

test("no braces", () => {
    let number = 0;
    do number++;
    while (number < 3);
    expect(number).toBe(3);
});

test("exception in test expression", () => {
    expect(() => {
        do {} while (foo);
    }).toThrow(ReferenceError);
});

test("automatic semicolon insertion", () => {
    expect("do {} while (false) foo").toEval();
});
