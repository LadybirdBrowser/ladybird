// Instructions hold the coordinate of a binding as a byte of hops and two bytes of binding index. Bindings past that
// are looked up by name, which must give the same results.

test("bindings of an environment past the first 65536", () => {
    const count = 65600;
    let source = "";
    for (let i = 0; i < count; ++i) source += `var p${i} = ${i};`;
    source += "let a = 1; var b = 2; function h() { return 7; }";
    source += "function padding() { return [";
    for (let i = 0; i < count; ++i) source += `p${i},`;
    source += "]; }";
    source += "return [function g() { a = a + b; b = 3; return [a, typeof a, h()]; }, padding];";
    const [g, padding] = new Function(source)();
    expect(g()).toEqual([3, "number", 7]);
    expect(g()).toEqual([6, "number", 7]);
    expect(g()).toEqual([9, "number", 7]);
    const values = padding();
    expect(values).toHaveLength(count);
    for (const i of [0, 65535, 65536, count - 1]) expect(values[i]).toBe(i);
});

test("bindings more than 255 environments out", () => {
    const depth = 270;
    let source = "";
    for (let i = 0; i < depth; ++i) source += `{ let x${i} = ${i}; (() => x${i});`;
    source += "return () => { x0 = x0 + 1; return [x0, x1, typeof x2, x268]; };";
    source += "}".repeat(depth);
    const f = new Function(source)();
    expect(f()).toEqual([1, 1, "number", 268]);
    expect(f()).toEqual([2, 1, "number", 268]);
});
