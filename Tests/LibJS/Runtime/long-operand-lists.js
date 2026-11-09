function range(count) {
    return Array.from({ length: count }, (_, index) => index);
}

function compile(...parts) {
    return new Function(...parts);
}

test("calls with more arguments than an instruction can hold", () => {
    const list = range(300).join(",");
    const count = compile(`function f() { return arguments.length; } return f(${list});`);
    expect(count()).toBe(300);
    const last = compile(`function f(...rest) { return rest[299]; } return f(${list});`);
    expect(last()).toBe(299);
    const constructed = compile(`function F() { this.n = arguments.length; } return new F(${list}).n;`);
    expect(constructed()).toBe(300);
    const evaluated = compile(`return eval("1 + 2", ${list});`);
    expect(evaluated()).toBe(3);
});

test("arguments in temporaries survive being gathered into an array", () => {
    const list = range(250).join(",");
    const f = compile("x", `function f() { return arguments[250]; } return f(${list}, x + 1);`);
    expect(f(41)).toBe(42);
});

test("array literals with more elements than an instruction can hold", () => {
    const elements = range(300)
        .map(index => `x + ${index}`)
        .join(",");
    const f = compile("x", `return [${elements}];`);
    const array = f(1);
    expect(array.length).toBe(300);
    expect(array[0]).toBe(1);
    expect(array[299]).toBe(300);
});

test("template objects with more strings than an instruction can hold", () => {
    const substitutions = range(150)
        .map(index => `a${index}\${x}`)
        .join("");
    const f = compile("x", `function tag(strings) { return strings; } return tag\`${substitutions}\`;`);
    const strings = f(1);
    expect(strings.length).toBe(151);
    expect(strings[0]).toBe("a0");
    expect(strings[149]).toBe("a149");
    expect(strings.raw[149]).toBe("a149");
    expect(f(2)).toBe(f(2));
});

test("object rest with more excluded names than an instruction can hold", () => {
    const names = range(250).map(index => `p${index}`);
    const f = compile("o", `const { ${names.join(",")}, ...rest } = o; return rest;`);
    const source = { kept: 1 };
    for (const name of names) source[name] = 0;
    expect(Object.keys(f(source))).toEqual(["kept"]);
});

test("classes with more elements than an instruction can hold", () => {
    const methods = range(250)
        .map(index => `m${index}() { return ${index}; }`)
        .join("\n");
    const C = compile(`return class { ${methods} ["computed"]() { return "c"; } static s = 1; };`)();
    const instance = new C();
    expect(instance.m0()).toBe(0);
    expect(instance.m249()).toBe(249);
    expect(instance.computed()).toBe("c");
    expect(C.s).toBe(1);
});

test("functions with more locals and constants than an instruction can name", () => {
    const count = 600;
    const declarations = range(count)
        .map(index => `let v${index} = ${index + 0.5};`)
        .join("\n");
    const sum = range(count)
        .map(index => `v${index}`)
        .join(" + ");
    const f = compile("a", `${declarations} v599 = a; try { throw v599; } catch (e) { v0 = e; } return [v0, ${sum}];`);
    const [caught, total] = f(7);
    expect(caught).toBe(7);
    expect(total).toBe((count * count) / 2 - 599.5 + 7 - 0.5 + 7);
});

test("functions with more names than 16 bits can index", () => {
    const count = 70000;
    const properties = range(count)
        .map(index => `p${index}:0`)
        .join(",");
    const f = compile(`const o = {${properties},last:42}; return o.last + o.p69999;`);
    expect(f()).toBe(42);
});
