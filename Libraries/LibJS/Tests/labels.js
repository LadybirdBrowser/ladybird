test("labelled plain scope", () => {
    notused: test: alsonotused: {
        let o = 1;
        expect(o).toBe(1);
        unused: break test;
        expect().fail();
    }
});

test("break on plain scope from inner scope", () => {
    notused: outer: alsonotused: {
        {
            unused: break outer;
        }
        expect().fail();
    }
});

test("labelled for loop with break", () => {
    let counter = 0;
    notused: outer: alsonotused: for (a of [1, 2, 3]) {
        for (b of [4, 5, 6]) {
            if (a === 2 && b === 5) break outer;
            counter++;
        }
    }
    expect(counter).toBe(4);
});

test("labelled for loop with continue", () => {
    let counter = 0;
    notused: outer: alsonotused: for (a of [1, 2, 3]) {
        for (b of [4, 5, 6]) {
            if (b === 6) continue outer;
            counter++;
        }
    }
    expect(counter).toBe(6);
});

test("continue label statement is not an iteration statement", () => {
    expect(() =>
        eval(`
outer: outer2: {
    for (;;) {
        continue outer;
    }
}`)
    ).toThrowWithMessage(
        SyntaxError,
        "labelled continue statement cannot use non iterating statement (line: 4, column: 18)"
    );

    expect(() =>
        eval(`
for (;;) {
    outer: outer2: {
        continue outer;
    }
}`)
    ).toThrowWithMessage(
        SyntaxError,
        "labelled continue statement cannot use non iterating statement (line: 4, column: 18)"
    );
});

test("break on try catch statement", () => {
    let entered = false;
    label1: label2: label3: try {
        entered = true;
        break label2;
        expect().fail();
    } catch (e) {
        expect().fail();
    }
    expect(entered).toBeTrue();
});

test("can break on every label", () => {
    let i = 0;
    label0: label1: label2: for (; i < 3; i++) {
        block: {
            break block;
            expect().fail();
        }
        if (i === 0) continue label0;
        if (i === 1) continue label1;
        if (i === 2) continue label2;
        expect().fail();
    }
    expect(i).toBe(3);
});

test("can use certain 'keywords' as labels", () => {
    let i = 0;

    yield: {
        i++;
        break yield;
        expect().fail();
    }

    await: {
        i++;
        break await;
        expect().fail();
    }

    async: {
        i++;
        break async;
        expect().fail();
    }

    let: {
        i++;
        break let;
        expect().fail();
    }

    // prettier-ignore
    l\u0065t: {
        i++;
        break let;
        expect().fail();
    }

    private: {
        i++;
        break private;
        expect().fail();
    }

    expect(i).toBe(6);

    expect(`const: { break const; }`).not.toEval();
});

test("can use certain 'keywords' even in strict mode", () => {
    "use strict";

    let i = 0;
    await: {
        i++;
        break await;
        expect().fail();
    }

    async: {
        i++;
        break async;
        expect().fail();
    }
    expect(i).toBe(2);

    expect(`'use strict'; let: { break let; }`).not.toEval();

    expect(`'use strict'; l\u0065t: { break l\u0065t; }`).not.toEval();
});

test("invalid label usage", () => {
    expect(() =>
        eval(`
            label: {
                (() => {
                    break label;
                });
            }
        `)
    ).toThrowWithMessage(SyntaxError, "Label 'label' not found");

    expect(() =>
        eval(`
            label: {
                while (false) {
                    continue label;
                }
            }
        `)
    ).toThrowWithMessage(
        SyntaxError,
        "labelled continue statement cannot use non iterating statement"
    );

    expect(() =>
        eval(`
            label: label: {
                break label;
            }
        `)
    ).toThrowWithMessage(SyntaxError, "Label 'label' has already been declared");
});
