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

test("mixed surrogate forms in named group names are syntax errors", () => {
    for (const source of [
        "/(?<a\\uD835\udcf8>.)/",
        "/(?<a\ud835\\uDCF8>.)/",
        "/(?<a\\uD835\\u{DCF8}>.)/",
        "/(?<a\\u{D835}\\uDCF8>.)/",
        "/(?<a\\u{D835}\\u{DCF8}>.)/",
    ]) {
        expect(source).not.toEval();
        expect(() => eval(source)).toThrow(SyntaxError);
        expect(() => new Function(source)).toThrow(SyntaxError);
    }
});

test("mixed surrogate forms in named backreferences are syntax errors", () => {
    for (const source of [
        "/(?<a\\uD835\\uDCF8>.)\\k<a\\uD835\udcf8>/",
        "/(?<a\\uD835\\uDCF8>.)\\k<a\ud835\\uDCF8>/",
        "/(?<a\\uD835\\uDCF8>.)\\k<a\\uD835\\u{DCF8}>/",
        "/(?<a\\uD835\\uDCF8>.)\\k<a\\u{D835}\\uDCF8>/",
        "/(?<a\\uD835\\uDCF8>.)\\k<a\\u{D835}\\u{DCF8}>/",
    ]) {
        expect(source).not.toEval();
        expect(() => eval(source)).toThrow(SyntaxError);
        expect(() => new Function(source)).toThrow(SyntaxError);
    }
});

test("large quantifier bounds clamp before regex literal order validation", () => {
    for (const source of [
        "/a{2147483648}/",
        "/a{2147483648,}/",
        "/a{2147483648,2147483647}/",
        "/a{2147483648,2147483648}/",
        "/a{99999999999999999999999999999999999999999999999999}/",
    ]) {
        expect(source).toEval();
        expect(() => eval(source)).not.toThrow();
        expect(() => new Function(source)).not.toThrow();
    }

    for (const source of ["/a{2147483647,2147483646}/", "/a{2147483648,2147483646}/"]) {
        expect(source).not.toEval();
        expect(() => eval(source)).toThrow(SyntaxError);
        expect(() => new Function(source)).toThrow(SyntaxError);
    }
});

test("negated v-mode classes containing nested strings are syntax errors", () => {
    for (const source of [
        "/[^[[\\p{Emoji_Keycap_Sequence}]]]/v",
        "/[^[[\\q{ab}]]]/v",
        String.raw`/[[[\p{Emoji_Presentation}]][\p{Math}]].*\p{Script=Hebrew}*\t[[^a-z]]?(?:\s{3}.+?[^[[\p{Emoji_Keycap_Sequence}]--[А-Я]][[\p{Script=Hebrew}]--[\p{Script=Latin}]]]??)/gv`,
    ]) {
        expect(source).not.toEval();
        expect(() => eval(source)).toThrow(SyntaxError);
        expect(() => new Function(source)).toThrow(SyntaxError);
    }
});

test("negated v-mode class set ops that eliminate strings are valid", () => {
    for (const source of [
        "/[^[[a-z]--[\\q{ab}]]]/v",
        "/[^[[\\q{ab}]&&[a-z]]]/v",
        "/[^[[\\q{ab}]--[\\q{ab}]]]/v",
        "/[^[[\\q{ab}]&&[\\q{cd}]]]/v",
    ]) {
        expect(source).toEval();
        expect(() => eval(source)).not.toThrow();
        expect(() => new Function(source)).not.toThrow();
    }
});

test("valid regex literals parse and execute correctly", () => {
    expect("/foo/g").toEval();
    expect("/[a-z]+/gims").toEval();
    expect("/(?:abc)/u").toEval();
    expect("/hello world/").toEval();
    expect("/foo/dgimsuy").toEval();
    expect("/foo/v").toEval();
});

test("named group names accept literal and escaped surrogate pairs in regex literals", () => {
    for (const source of ["/(?<a\ud835\udcf8>.)/", "/(?<a\\uD835\\uDCF8>.)/", "/(?<a\\u{1D4F8}>.)/"]) {
        expect(source).toEval();
        expect(() => eval(source)).not.toThrow();
        expect(() => new Function(source)).not.toThrow();
    }
});

test("named backreferences accept literal and escaped surrogate pairs in regex literals", () => {
    for (const source of [
        "/(?<a\ud835\udcf8>.)\\k<a\ud835\udcf8>/",
        "/(?<a\\uD835\\uDCF8>.)\\k<a\\uD835\\uDCF8>/",
        "/(?<a\\u{1D4F8}>.)\\k<a\\u{1D4F8}>/",
    ]) {
        expect(source).toEval();
        expect(() => eval(source)).not.toThrow();
        expect(() => new Function(source)).not.toThrow();
    }
});

test("legacy \\k identity escapes remain valid regex literals", () => {
    const source = "/\\k<\\uDC00>/";

    expect(source).toEval();
    expect(() => eval(source)).not.toThrow();
    expect(() => new Function(source)).not.toThrow();
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
