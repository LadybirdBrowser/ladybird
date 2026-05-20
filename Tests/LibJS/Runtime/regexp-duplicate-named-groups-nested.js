test("duplicate nested named capture groups in same alternative are rejected", () => {
    expect(() => eval("/(?<a>.)((?<a>.)|(?<b>.))/")).toThrow(SyntaxError);
    expect(() => eval("/x(?<a>.)((((?<a>.)|(?<a>.))|(?<a>.)|(?<a>.))|(?<a>.))|(?<a>.)y/")).toThrow(SyntaxError);
    expect(() => eval("/x((?<a>.)(((?<a>.)|(?<a>.))|(?<a>.)|(?<a>.))|(?<a>.))|(?<a>.)y/")).toThrow(SyntaxError);
    expect(() => eval("/(?<a>.)((?<a>.))/")).toThrow(SyntaxError);
    expect(() => eval("/(?<b>.)((?<a>.)|(?<b>.))/")).toThrow(SyntaxError);
    expect(() => eval("/x(((?<a>.)((?<a>.)|(?<a>.))|(?<a>.)|(?<a>.))|(?<a>.))|(?<a>.)y/")).toThrow(SyntaxError);
    expect(() => eval("/(?<a>(?<a>.)|.)/")).toThrow(SyntaxError);
    expect(() => eval("/(?<a>.|(?<b>.(?<b>.)|.))/")).toThrow(SyntaxError);
});
