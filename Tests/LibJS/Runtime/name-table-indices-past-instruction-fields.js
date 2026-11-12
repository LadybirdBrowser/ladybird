// Instructions hold the indices of names in 16 bits. Larger ones go in another instruction before them, which must
// give the same results.

const count = 65400;

test("identifiers of an executable past the first 65280", () => {
    let source = "";
    for (let i = 0; i < count; ++i) source += `typeof g${i};`;
    source += "wideGlobal = wideGlobal + 1;";
    source += "const wideFunction = function () { return wideGlobal; };";
    source += "const WideClass = class { static value() { return wideGlobal; } };";
    source += "let error; try { wideMissing; } catch (e) { error = e; }";
    source += "return [wideFunction.name, wideFunction(), WideClass.name, WideClass.value(), error.message];";
    globalThis.wideGlobal = 1;
    const f = new Function(source);
    expect(f()).toEqual(["wideFunction", 2, "WideClass", 2, "'wideMissing' is not defined"]);
    expect(f()).toEqual(["wideFunction", 3, "WideClass", 3, "'wideMissing' is not defined"]);
    delete globalThis.wideGlobal;
});

test("identifiers past the first 65280 in a function with more frame slots than operands can name", () => {
    let source = "";
    for (let i = 0; i < 300; ++i) source += `let l${i} = ${i};`;
    for (let i = 0; i < count; ++i) source += `typeof g${i};`;
    source += "wideSpilled = l299; return [wideSpilled, l0 + l150];";
    const f = new Function(source);
    expect(f()).toEqual([299, 150]);
    expect(globalThis.wideSpilled).toBe(299);
    delete globalThis.wideSpilled;
});

test("property keys of an executable past the first 65280", () => {
    let source = "const o = {};";
    for (let i = 0; i < count; ++i) source += `o.k${i};`;
    source += "o.wideKey = 1; o.wideKey += 1; return [o.wideKey, delete o.wideKey, o.wideKey];";
    const f = new Function(source);
    expect(f()).toEqual([2, true, undefined]);
    expect(f()).toEqual([2, true, undefined]);
});

test("strings of an executable past the first 65280", () => {
    let source = "const regexps = [];";
    for (let i = 0; i < count; ++i) source += `/s${i}/;`;
    source += "return /wide/gi;";
    const f = new Function(source);
    const regexp = f();
    expect(regexp.source).toBe("wide");
    expect(regexp.flags).toBe("gi");
});
