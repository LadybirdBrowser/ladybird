test("mixing coalescing and logical operators isn't allowed", () => {
    expect("if (0) a ?? b || c").not.toEval();
    expect("if (0) a ?? b && c").not.toEval();
    expect("if (0) a ?? b * c || d").not.toEval();
    expect("if (0) a ?? b * c && d").not.toEval();
    expect("if (0) a && b ?? c").not.toEval();
    expect("if (0) a || b ?? c").not.toEval();
    expect("if (0) a && b * c ?? d").not.toEval();
    expect("if (0) a || b * c ?? d").not.toEval();
});

test("mixing coalescing and logical operators with parens", () => {
    expect("if (0) a ?? (b || c)").toEval();
    expect("if (0) (a ?? b) && c").toEval();
    expect("if (0) a ?? (b * c || d)").toEval();
    expect("if (0) (a ?? b * c) && d").toEval();
    expect("if (0) a && (b ?? c)").toEval();
    expect("if (0) (a || b) ?? c").toEval();
    expect("if (0) a && (b * c) ?? d").not.toEval();
    expect("if (0) a || (b * c) ?? d").not.toEval();
});

test("mixing coalescing and logical operators in ternary expressions", () => {
    expect("0 || 0 ? 0 : 0 ?? 0").toEval();
    expect("0 ?? 0 ? 0 : 0 || 0").toEval();
    expect("0 ? 0 || 0 : 0 ?? 0").toEval();
    expect("0 ? 0 ?? 0 : 0 || 0").toEval();
    expect("0 && 0 ? 0 ?? 0 : 0 || 0").toEval();
    expect("0 ?? 0 ? 0 && 0 : 0 || 0").toEval();
    expect("0 ?? 0 ? 0 || 0 : 0 && 0").toEval();
    expect("0 || 0 ? 0 ?? 0 : 0 && 0").toEval();
    expect("0 && 0 ? 0 || 0 : 0 ?? 0").toEval();
    expect("0 || 0 ? 0 && 0 : 0 ?? 0").toEval();
});

test("mixing coalescing and logical operators when 'in' isn't allowed", () => {
    expect("for (a ?? b || c in a; false;);").not.toEval();
    expect("for (a ?? b && c in a; false;);").not.toEval();
    expect("for (a || b ?? c in a; false;);").not.toEval();
    expect("for (a && b ?? c in a; false;);").not.toEval();
});
