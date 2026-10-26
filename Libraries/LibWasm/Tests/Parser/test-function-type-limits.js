function uleb(value) {
    const bytes = [];
    do {
        const byte = value & 0x7f;
        value >>= 7;
        bytes.push(value ? byte | 0x80 : byte);
    } while (value);
    return bytes;
}

function moduleWithFunctionType(paramCount, resultCount) {
    const i32 = 0x7f;
    const types = [
        1,
        0x60,
        ...uleb(paramCount),
        ...Array(paramCount).fill(i32),
        ...uleb(resultCount),
        ...Array(resultCount).fill(i32),
    ];
    return new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 1, ...uleb(types.length), ...types]);
}

test("a function type has at most 1000 parameters", () => {
    parseWebAssemblyModule(moduleWithFunctionType(1000, 0));
    expect(() => parseWebAssemblyModule(moduleWithFunctionType(1001, 0))).toThrow(SyntaxError);
});

test("a function type has at most 1000 results", () => {
    parseWebAssemblyModule(moduleWithFunctionType(0, 1000));
    expect(() => parseWebAssemblyModule(moduleWithFunctionType(0, 1001))).toThrow(SyntaxError);
});
