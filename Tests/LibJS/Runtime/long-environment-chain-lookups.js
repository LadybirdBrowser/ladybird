test("a global looked up through a very long chain of function environments", () => {
    let next = function (code, eval) {
        return eval(code);
    };
    for (let i = 0; i < 50000; i++) next = next("(function (code, eval) { return eval(code); })", eval);
    expect(next("typeof Array", eval)).toBe("function");
});

test("an assignment through more block scopes than a coordinate holds, near the call stack size limit", () => {
    let source = "let v = 7; (() => v);";
    for (let i = 0; i < 300; i++) source += `{ let b${i} = 0; (() => b${i});`;
    source += "v = 8; return v;" + "}".repeat(300);
    const deep = new Function(source);
    expect(deep()).toBe(8);

    const results = new Set();
    function recurse() {
        try {
            Reflect.apply(recurse, null, []);
        } catch {}
        try {
            results.add(deep());
        } catch {}
    }
    recurse();
    expect([...results]).toEqual([8]);
});
