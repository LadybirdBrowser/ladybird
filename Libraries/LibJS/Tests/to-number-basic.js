test("non-numeric primitives", () => {
    expect(+false).toBe(0);
    expect(-false).toBe(-0);
    expect(+true).toBe(1);
    expect(-true).toBe(-1);
    expect(+null).toBe(0);
    expect(-null).toBe(-0);
    expect(+undefined).toBeNaN();
    expect(-undefined).toBeNaN();
});

test("arrays", () => {
    expect(+[]).toBe(0);
    expect(-[]).toBe(-0);
    expect(+[,]).toBe(0);
    expect(-[,]).toBe(-0);
    expect(+[null]).toBe(0);
    expect(-[null]).toBe(-0);
    expect(+[undefined]).toBe(0);
    expect(-[undefined]).toBe(-0);
    expect(+[[[[[]]]]]).toBe(0);
    expect(-[[[[[]]]]]).toBe(-0);
    expect(+[[[[[42]]]]]).toBe(42);
    expect(-[[[[[42]]]]]).toBe(-42);

    expect(+[, , ,]).toBeNaN();
    expect(-[, , ,]).toBeNaN();
    expect(+[undefined, undefined]).toBeNaN();
    expect(-[undefined, undefined]).toBeNaN();
    expect(+[1, 2, 3]).toBeNaN();
    expect(-[1, 2, 3]).toBeNaN();
    expect(+[[[["foo"]]]]).toBeNaN();
    expect(-[[[["foo"]]]]).toBeNaN();
});

test("strings", () => {
    expect(+"").toBe(0);
    expect(-"").toBe(-0);
    expect(+"42").toBe(42);
    expect(-"42").toBe(-42);
    expect(+"1.23").toBe(1.23);
    expect(-"1.23").toBe(-1.23);

    expect(+"foo").toBeNaN();
    expect(-"foo").toBeNaN();
});

test("numbers", () => {
    expect(+42).toBe(42);
    expect(-42).toBe(-42);
    expect(+1.23).toBe(1.23);
    expect(-1.23).toBe(-1.23);
});

test("infinity", () => {
    expect(+"Infinity").toBe(Infinity);
    expect(+"+Infinity").toBe(Infinity);
    expect(+"-Infinity").toBe(-Infinity);
    expect(-"Infinity").toBe(-Infinity);
    expect(-"+Infinity").toBe(-Infinity);
    expect(-"-Infinity").toBe(Infinity);
});

test("space and space-like escapes", () => {
    expect(+"  \r  \t \n ").toBe(0);
    expect(+"  \n  \t    Infinity   \r   ").toBe(Infinity);
    expect(+"\r     \n1.23   \t\t\t  \n").toBe(1.23);
});

test("object literals", () => {
    expect(+{}).toBeNaN();
    expect(-{}).toBeNaN();
    expect(+{ a: 1 }).toBeNaN();
    expect(-{ a: 1 }).toBeNaN();
});
