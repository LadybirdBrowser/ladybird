test("'break' syntax errors", () => {
    expect("break").not.toEval();
    expect("break label").not.toEval();
    expect("{ break }").not.toEval();
    expect("{ break label }").not.toEval();
    expect("label: { break label }").toEval();
});

test("'continue' syntax errors", () => {
    expect("continue").not.toEval();
    expect("continue label").not.toEval();
    expect("{ continue }").not.toEval();
    expect("{ continue label }").not.toEval();
    expect("label: { continue label }").not.toEval();

    expect("switch (true) { case true: continue; }").not.toEval();
});
