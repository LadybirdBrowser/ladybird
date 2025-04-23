test("JSON.rawJSON basic functionality", () => {
    expect(Object.isExtensible(JSON.rawJSON)).toBeTrue();
    expect(typeof JSON.rawJSON).toBe("function");
    expect(Object.getPrototypeOf(JSON.rawJSON)).toBe(Function.prototype);
    expect(Object.getOwnPropertyDescriptor(JSON.rawJSON, "prototype")).toBeUndefined();

    expect(JSON.stringify(JSON.rawJSON(1))).toBe("1");
    expect(JSON.stringify(JSON.rawJSON(1.1))).toBe("1.1");
    expect(JSON.stringify(JSON.rawJSON(-1))).toBe("-1");
    expect(JSON.stringify(JSON.rawJSON(-1.1))).toBe("-1.1");
    expect(JSON.stringify(JSON.rawJSON(1.1e1))).toBe("11");
    expect(JSON.stringify(JSON.rawJSON(1.1e-1))).toBe("0.11");
    expect(JSON.stringify(JSON.rawJSON(null))).toBe("null");
    expect(JSON.stringify(JSON.rawJSON(true))).toBe("true");
    expect(JSON.stringify(JSON.rawJSON(false))).toBe("false");
    expect(JSON.stringify(JSON.rawJSON('"foo"'))).toBe('"foo"');

    expect(JSON.stringify({ 42: JSON.rawJSON(37) })).toBe('{"42":37}');
    expect(JSON.stringify({ x: JSON.rawJSON(1), y: JSON.rawJSON(2) })).toBe('{"x":1,"y":2}');
    expect(JSON.stringify({ x: { x: JSON.rawJSON(1), y: JSON.rawJSON(2) } })).toBe(
        '{"x":{"x":1,"y":2}}'
    );

    expect(JSON.stringify([JSON.rawJSON(1), JSON.rawJSON(1.1)])).toBe("[1,1.1]");
    expect(
        JSON.stringify([
            JSON.rawJSON('"1"'),
            JSON.rawJSON(true),
            JSON.rawJSON(null),
            JSON.rawJSON(false),
        ])
    ).toBe('["1",true,null,false]');
    expect(JSON.stringify([{ x: JSON.rawJSON(1), y: JSON.rawJSON(1) }])).toBe('[{"x":1,"y":1}]');
});

test("JSON.rawJSON error cases", () => {
    expect(() => JSON.rawJSON(Symbol("123"))).toThrow(TypeError);
    expect(() => JSON.rawJSON(undefined)).toThrow(SyntaxError);
    expect(() => JSON.rawJSON({})).toThrow(SyntaxError);
    expect(() => JSON.rawJSON([])).toThrow(SyntaxError);

    const illegalChars = ["\n", "\t", "\r", " "];

    illegalChars.forEach(char => {
        expect(() => {
            JSON.rawJSON(`${char}123`);
        }).toThrow(SyntaxError);

        expect(() => {
            JSON.rawJSON(`123${char}`);
        }).toThrow(SyntaxError);
    });

    expect(() => {
        JSON.rawJSON("");
    }).toThrow(SyntaxError);
});
