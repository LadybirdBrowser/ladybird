test("regular exponentiation", () => {
    expect(2 ** 0).toBe(1);
    expect(2 ** 1).toBe(2);
    expect(2 ** 2).toBe(4);
    expect(2 ** 3).toBe(8);
    expect(3 ** 2).toBe(9);
    expect(0 ** 0).toBe(1);
    expect(2 ** (3 ** 2)).toBe(512);
    expect(2 ** (3 ** 2)).toBe(512);
    expect((2 ** 3) ** 2).toBe(64);
});

test("exponentiation with negatives", () => {
    expect(2 ** -3).toBe(0.125);
    expect((-2) ** 3).toBe(-8);

    expect("-2 ** 3").not.toEval();
});

test("exponentiation with PlusPlus and MinusMinus", () => {
    let value = 5;
    // prettier-ignore
    expect(++value ** 2).toBe(36);

    value = 5;
    expect((++value) ** 2).toBe(36);

    value = 5;
    // prettier-ignore
    expect(--value ** 2).toBe(16);

    value = 5;
    expect((--value) ** 2).toBe(16);

    expect("++5 ** 2").not.toEval();
    expect("--5 ** 2").not.toEval();
});

test("exponentiation with non-numeric primitives", () => {
    expect("2" ** "3").toBe(8);
    expect("" ** []).toBe(1);
    expect([] ** null).toBe(1);
    expect(null ** null).toBe(1);
    expect(undefined ** null).toBe(1);
});

test("exponentiation that produces NaN", () => {
    expect(NaN ** 2).toBeNaN();
    expect(2 ** NaN).toBeNaN();
    expect(undefined ** 2).toBeNaN();
    expect(2 ** undefined).toBeNaN();
    expect(null ** undefined).toBeNaN();
    expect(2 ** "foo").toBeNaN();
    expect("foo" ** 2).toBeNaN();
});

test("exponentiation with infinities", () => {
    expect((-1) ** Infinity).toBeNaN();
    expect(0 ** Infinity).toBe(0);
    expect(1 ** Infinity).toBeNaN();
    expect((-1) ** -Infinity).toBeNaN();
    expect(0 ** -Infinity).toBe(Infinity);
    expect(1 ** -Infinity).toBeNaN();
    expect(Infinity ** -1).toBe(0);
    expect(Infinity ** 0).toBe(1);
    expect(Infinity ** 1).toBe(Infinity);
    expect((-Infinity) ** -1).toBe(-0);
    expect((-Infinity) ** 0).toBe(1);
    expect((-Infinity) ** 1).toBe(-Infinity);
});

test("unary expression before exponentiation with brackets", () => {
    expect((!1) ** 2).toBe(0);
    expect((~5) ** 2).toBe(36);
    expect((+5) ** 2).toBe(25);
    expect((-5) ** 2).toBe(25);
});
