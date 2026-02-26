test("invalid regex literal pattern is a syntax error", () => {
    expect("/[/").not.toEval();
    expect("/(/").not.toEval();
    expect("/(?/").not.toEval();
    expect("/\\p{Invalid}/u").not.toEval();
});

test("duplicate regex flags are a syntax error", () => {
    expect("/foo/gg").not.toEval();
    expect("/foo/ii").not.toEval();
    expect("/foo/gig").not.toEval();
});

test("invalid regex flag is a syntax error", () => {
    expect("/foo/x").not.toEval();
    expect("/foo/Q").not.toEval();
});

test("regex literal errors in eval", () => {
    expect(() => eval("/[/")).toThrow(SyntaxError);
    expect(() => eval("/foo/gg")).toThrow(SyntaxError);
    expect(() => eval("/foo/x")).toThrow(SyntaxError);
});

test("regex literal errors in new Function()", () => {
    expect(() => new Function("/[/")).toThrow(SyntaxError);
    expect(() => new Function("/foo/gg")).toThrow(SyntaxError);
    expect(() => new Function("/foo/x")).toThrow(SyntaxError);
});

test("valid regex literals parse and execute correctly", () => {
    expect("/foo/g").toEval();
    expect("/[a-z]+/gims").toEval();
    expect("/(?:abc)/u").toEval();
    expect("/hello world/").toEval();
    expect("/foo/dgimsuy").toEval();
    expect("/foo/v").toEval();
});

test("regex literals work inside functions", () => {
    function f() {
        return /foo/g;
    }
    expect(f().test("foo")).toBeTrue();
    expect(f().test("bar")).toBeFalse();
});

test("regex literals work inside arrow functions", () => {
    const f = () => /bar/i;
    expect(f().test("BAR")).toBeTrue();
    expect(f().test("baz")).toBeFalse();
});

test("multiple regex literals in the same scope", () => {
    const a = /abc/;
    const b = /def/g;
    const c = /ghi/i;
    expect(a.test("abc")).toBeTrue();
    expect(b.test("def")).toBeTrue();
    expect(c.test("GHI")).toBeTrue();
    expect(a.test("def")).toBeFalse();
});
