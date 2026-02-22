test("for-of RHS must be AssignmentExpression, not comma expression", () => {
    expect("for (var x of 1, 2) {}").not.toEval();
    expect("for (let x of 1, 2) {}").not.toEval();
    expect("for (const x of 1, 2) {}").not.toEval();
    expect("for (x of 1, 2) {}").not.toEval();

    // Valid: parenthesized comma expression is fine (it's a single AssignmentExpression)
    expect("for (var x of (1, 2)) {}").toEval();
});

test("for-of may not start with let", () => {
    expect("for (let.x of []) {}").not.toEval();
});
