// Regression test for a bytecode-generator bug where a break/continue that crossed a for-of or try/finally boundary on
// its way to an outer loop failed to restore the lexical environment of an intervening block scope. See #9251.

test("labelled continue out of a for-of nested in a block scope", () => {
    const values = [];
    const list = [10, 20, 30];
    let index = 0;
    const readIndex = () => index;
    const readListLength = () => list.length;
    main: for (; index < list.length; ) {
        const current = list[index];
        const readCurrent = () => current;
        for (const step of [1, 2]) {
            if (step === 2) {
                values.push(readCurrent());
                index++;
                continue main;
            }
        }
    }
    expect(values).toEqual([10, 20, 30]);
    expect(readIndex()).toBe(3);
    expect(readListLength()).toBe(3);
});

test("unlabelled continue out of a try/finally nested in a block scope", () => {
    const values = [];
    const list = [10, 20, 30];
    let index = 0;
    const readIndex = () => index;
    const readListLength = () => list.length;
    for (; index < list.length; ) {
        const current = list[index];
        const readCurrent = () => current;
        index++;
        try {
            continue;
        } finally {
            values.push(readCurrent());
        }
    }
    expect(values).toEqual([10, 20, 30]);
    expect(readIndex()).toBe(3);
    expect(readListLength()).toBe(3);
});

test("labelled break out of a for-of nested in a block scope", () => {
    const values = [];
    const list = [10, 20, 30];
    let index = 0;
    const readIndex = () => index;
    const readListLength = () => list.length;
    main: for (; index < list.length; ) {
        const current = list[index];
        const readCurrent = () => current;
        for (const step of [1, 2]) {
            if (step === 2) {
                values.push(readCurrent());
                if (index === 1) break main;
                index++;
                continue main;
            }
        }
    }
    expect(values).toEqual([10, 20]);
    expect(readIndex()).toBe(1);
    expect(readListLength()).toBe(3);
});
