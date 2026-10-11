test("large frames with many arithmetic slow paths", () => {
    let body = "let result = input;";
    for (let i = 0; i < 1000; ++i) body += `result = result + ${i + 0.123456789};`;
    body += "return result;";
    const sum = new Function("input", body);
    const reference = new Function("input", body);
    jit.neverCompile(reference);
    jit.prepare(sum);
    expect(sum(1)).toBe(reference(1));
    expect(sum("x")).toBe(reference("x"));
    expect(jit.compile(sum)).toBe(jit.enabled);
    expect(sum(1)).toBe(reference(1));
    expect(sum("x")).toBe(reference("x"));
    expect(sum({ valueOf: () => 2 })).toBe(reference(2));
    expect(() => sum(Symbol())).toThrow(TypeError);
    expect(sum(3)).toBe(reference(3));
});
