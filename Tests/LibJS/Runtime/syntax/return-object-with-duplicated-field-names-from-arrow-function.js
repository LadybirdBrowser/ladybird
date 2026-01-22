test("returning object with duplicated member names (i) should not throw exception", () => {
    expect(`
const f = (i) => ({
    obj: { a: { x: i }, b: { x: i } },
    g: () => {},
});

f(123);
    `).toEval();
});
