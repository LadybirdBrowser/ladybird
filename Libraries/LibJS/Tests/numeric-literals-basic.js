// FIXME: Some of the test cases below are duplicated, presumably to test
// uppercase as well which then got lowercased by Prettier at some point.

test("hex literals", () => {
    expect(0xff).toBe(255);
    expect(0xff).toBe(255);
});

test("octal literals", () => {
    expect(0o10).toBe(8);
    expect(0o10).toBe(8);
    expect(010).toBe(8);
    expect(089).toBe(89);
});

test("binary literals", () => {
    expect(0b10).toBe(2);
    expect(0b10).toBe(2);
});

test("exponential literals", () => {
    expect(1e3).toBe(1000);
    expect(1e3).toBe(1000);
    expect(1e-3).toBe(0.001);
    expect(1e1).toBe(10);
    expect(0.1e1).toBe(1);
    expect(0.1e1).toBe(1);
    expect(0.1e1).toBe(1);
    expect(0.1e1).toBe(1);
});

test("decimal numbers", () => {
    expect(1).toBe(1);
    expect(0.1).toBe(0.1);
});

test("accessing properties of decimal numbers", () => {
    Number.prototype.foo = "foo";
    expect((1).foo).toBe("foo");
    expect((1.1).foo).toBe("foo");
    expect((0.1).foo).toBe("foo");
});

test("invalid numeric literals", () => {
    expect("1e").not.toEval();
    expect("0x").not.toEval();
    expect("0b").not.toEval();
    expect("0o").not.toEval();
    expect("'use strict'; 0755").not.toEval();
    expect("1in[]").not.toEval();
    expect("2instanceof foo").not.toEval();
});
