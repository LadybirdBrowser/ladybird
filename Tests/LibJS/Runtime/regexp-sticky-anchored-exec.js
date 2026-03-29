test("sticky literal exec and test stay anchored at lastIndex", () => {
    let literal = /foobar/y;

    literal.lastIndex = 0;
    expect(literal.exec("..foobar")).toBeNull();
    expect(literal.lastIndex).toBe(0);

    literal.lastIndex = 2;
    let match = literal.exec("..foobar");
    expect(match).not.toBeNull();
    expect(match[0]).toBe("foobar");
    expect(match.index).toBe(2);
    expect(match.input).toBe("..foobar");
    expect(literal.lastIndex).toBe(8);

    literal.lastIndex = 0;
    expect(literal.test("..foobar")).toBeFalse();
    expect(literal.lastIndex).toBe(0);

    literal.lastIndex = 2;
    expect(literal.test("..foobar")).toBeTrue();
    expect(literal.lastIndex).toBe(8);
});

test("sticky word-boundary literal stays anchored at lastIndex", () => {
    let word = /\bfoo\b/y;

    word.lastIndex = 2;
    expect(word.exec("xx foo yy")).toBeNull();
    expect(word.lastIndex).toBe(0);

    word.lastIndex = 3;
    let match = word.exec("xx foo yy");
    expect(match).not.toBeNull();
    expect(match[0]).toBe("foo");
    expect(match.index).toBe(3);
    expect(word.lastIndex).toBe(6);

    word.lastIndex = 2;
    expect(word.test("xx foo yy")).toBeFalse();
    expect(word.lastIndex).toBe(0);

    word.lastIndex = 3;
    expect(word.test("xx foo yy")).toBeTrue();
    expect(word.lastIndex).toBe(6);
});

test("sticky literal alternation respects source order at lastIndex", () => {
    let first_shorter = /foo|foobar/y;
    first_shorter.lastIndex = 0;
    let shorter_match = first_shorter.exec("foobar");
    expect(shorter_match).not.toBeNull();
    expect(shorter_match[0]).toBe("foo");
    expect(first_shorter.lastIndex).toBe(3);

    let first_longer = /foobar|foo/y;
    first_longer.lastIndex = 0;
    let longer_match = first_longer.exec("foobar");
    expect(longer_match).not.toBeNull();
    expect(longer_match[0]).toBe("foobar");
    expect(first_longer.lastIndex).toBe(6);

    let mismatch = /foo|bar/y;
    mismatch.lastIndex = 0;
    expect(mismatch.exec("xxbar")).toBeNull();
    expect(mismatch.lastIndex).toBe(0);

    mismatch.lastIndex = 2;
    let bar_match = mismatch.exec("xxbar");
    expect(bar_match).not.toBeNull();
    expect(bar_match[0]).toBe("bar");
    expect(bar_match.index).toBe(2);
    expect(mismatch.lastIndex).toBe(5);
});

test("sticky character classes and builtin classes stay anchored at lastIndex", () => {
    let digits = /\d/y;
    digits.lastIndex = 0;
    expect(digits.exec("a1")).toBeNull();
    expect(digits.lastIndex).toBe(0);

    digits.lastIndex = 1;
    let digit_match = digits.exec("a1");
    expect(digit_match).not.toBeNull();
    expect(digit_match[0]).toBe("1");
    expect(digit_match.index).toBe(1);
    expect(digits.lastIndex).toBe(2);

    let upper = /[A-Z]/y;
    upper.lastIndex = 0;
    expect(upper.test("xA")).toBeFalse();
    expect(upper.lastIndex).toBe(0);

    upper.lastIndex = 1;
    expect(upper.test("xA")).toBeTrue();
    expect(upper.lastIndex).toBe(2);
});

test("sticky greedy quantifiers only begin at lastIndex", () => {
    let spaces = /\s+/y;

    spaces.lastIndex = 0;
    expect(spaces.exec("x   ")).toBeNull();
    expect(spaces.lastIndex).toBe(0);

    spaces.lastIndex = 1;
    let space_match = spaces.exec("x   ");
    expect(space_match).not.toBeNull();
    expect(space_match[0]).toBe("   ");
    expect(space_match.index).toBe(1);
    expect(spaces.lastIndex).toBe(4);

    let not_a = /[^a]+/y;
    not_a.lastIndex = 0;
    expect(not_a.test("aBC")).toBeFalse();
    expect(not_a.lastIndex).toBe(0);

    not_a.lastIndex = 1;
    expect(not_a.test("aBC")).toBeTrue();
    expect(not_a.lastIndex).toBe(3);
});

test("sticky tokenizer-style identifier and decimal patterns stay anchored", () => {
    let identifier = /[_-]?[A-Za-z][0-9A-Z_a-z-]*/y;

    identifier.lastIndex = 0;
    expect(identifier.exec(".identifier")).toBeNull();
    expect(identifier.lastIndex).toBe(0);

    identifier.lastIndex = 1;
    let identifier_match = identifier.exec(".identifier");
    expect(identifier_match).not.toBeNull();
    expect(identifier_match[0]).toBe("identifier");
    expect(identifier_match.index).toBe(1);
    expect(identifier.lastIndex).toBe(11);

    let decimal = /-?(?=[0-9]*\.|[0-9]+[eE])(([0-9]+\.[0-9]*|[0-9]*\.[0-9]+)([Ee][-+]?[0-9]+)?|[0-9]+[Ee][-+]?[0-9]+)/y;

    decimal.lastIndex = 0;
    expect(decimal.test("type 1.25e3")).toBeFalse();
    expect(decimal.lastIndex).toBe(0);

    decimal.lastIndex = 5;
    expect(decimal.test("type 1.25e3")).toBeTrue();
    expect(decimal.lastIndex).toBe(11);
});

test("sticky tokenizer-style block comments stay anchored", () => {
    let comment = /\/\*[\s\S]*?\*\//y;

    comment.lastIndex = 0;
    expect(comment.exec("x/* comment */")).toBeNull();
    expect(comment.lastIndex).toBe(0);

    comment.lastIndex = 1;
    let match = comment.exec("x/* comment */");
    expect(match).not.toBeNull();
    expect(match[0]).toBe("/* comment */");
    expect(match.index).toBe(1);
    expect(comment.lastIndex).toBe(14);
});
