test("completion value of for-in over undefined or null with a var declaration head", () => {
    expect(eval("1; for (var a in undefined) { }")).toBeUndefined();
    expect(eval("2; for (var b in undefined) { 3; }")).toBeUndefined();
    expect(eval("4; for (var c in null) { }")).toBeUndefined();
    expect(eval("5; for (var d in null) { 6; }")).toBeUndefined();
});

test("completion value of for-in over undefined or null with an expression head", () => {
    expect(eval("var a; 1; for (a in undefined) { }")).toBeUndefined();
    expect(eval("var b; 2; for (b in undefined) { 3; }")).toBeUndefined();
    expect(eval("var c; 4; for (c in null) { }")).toBeUndefined();
    expect(eval("var d; 5; for (d in null) { 6; }")).toBeUndefined();
});

test("completion value of for-in over undefined or null with a lexical declaration head", () => {
    expect(eval("1; for (let a in undefined) { }")).toBeUndefined();
    expect(eval("2; for (const b in null) { 3; }")).toBeUndefined();
});

test("completion value of labelled for-in over undefined or null", () => {
    expect(eval("1; outer: for (var a in undefined) { }")).toBeUndefined();
    expect(eval("2; outer: for (var b in null) { 3; }")).toBeUndefined();
});

test("completion value of for-in over an object without enumerable properties", () => {
    expect(eval("1; for (var a in {}) { }")).toBeUndefined();
    expect(eval("2; for (var b in { x: 1 }) { 3; }")).toBe(3);
});
