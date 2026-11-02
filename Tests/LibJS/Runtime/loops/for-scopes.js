test("var in for head", () => {
    for (var v = 5; false; );
    expect(v).toBe(5);
});

test("let in for head", () => {
    for (let l = 5; false; );
    expect(() => {
        l;
    }).toThrowWithMessage(ReferenceError, "'l' is not defined");
});

test("const in for head", () => {
    for (const c = 5; false; );
    expect(() => {
        c;
    }).toThrowWithMessage(ReferenceError, "'c' is not defined");
});

test("let variables captured by value", () => {
    let result = "";
    let functionList = [];
    for (let i = 0; i < 9; i++) {
        result += i;
        functionList.push(function () {
            return i;
        });
    }
    for (let i = 0; i < functionList.length; i++) {
        result += functionList[i]();
    }

    expect(result).toEqual("012345678012345678");
});

test("let variables captured by value when the body continues", () => {
    const functions = [];
    for (let i = 0; i < 3; i++) {
        functions.push(() => i);
        continue;
    }
    expect(functions.map(f => f())).toEqual([0, 1, 2]);

    functions.length = 0;
    for (let i = 0; i < 4; i++) {
        functions.push(() => i);
        if (i % 2 === 0) continue;
    }
    expect(functions.map(f => f())).toEqual([0, 1, 2, 3]);

    functions.length = 0;
    outer: for (let i = 0; i < 3; i++) {
        for (;;) {
            functions.push(() => i);
            continue outer;
        }
    }
    expect(functions.map(f => f())).toEqual([0, 1, 2]);
});
