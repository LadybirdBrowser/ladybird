test("empty semicolon statements", () => {
    expect(";;;").toEval();
});

test("if with no body", () => {
    expect("if (true);").toEval();
});

test("chained ifs with no bodies", () => {
    expect("if (false); else if (false); else;").toEval();
});

test("while with no body", () => {
    expect("while (false);").toEval();
});

test("do while with no body", () => {
    expect("do; while (false);").toEval();
});
