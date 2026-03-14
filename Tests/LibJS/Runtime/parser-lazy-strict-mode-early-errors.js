test("strict-only early errors reject eager function bodies", () => {
    const source = 'function f(){ "use strict"; with({}){} }';

    expect(source).not.toEval();
});

test("strict-only early errors apply inside lazy inner functions", () => {
    const source = 'function outer(){ function inner(){ "use strict"; with({}){} } }';

    expect(source).not.toEval();
});

test("strict mode inherited from outer scope applies inside lazy inner functions", () => {
    const source = '"use strict"; function outer(){ function inner(){ with({}){} } }';

    expect(source).not.toEval();
});
