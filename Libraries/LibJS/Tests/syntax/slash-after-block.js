test("slash token resolution in lexer", () => {
    expect(`{ blah.blah; }\n/foo/`).toEval();
    expect(`{ blah?.blah.blah; }\n/foo/`).toEval();
    expect("``/foo/").not.toEval();
    expect("1/foo/").not.toEval();
    expect("1/foo").toEval();

    expect("{} /foo/").toEval();
    expect("{} /=/").toEval();
    expect("{} /=a/").toEval();
    expect("{} /* */ /=a/").toEval();
    expect("{} /* /a/ */ /=a/").toEval();

    expect("(function () {} / 1)").toEval();
    expect("(function () {} / 1)").toEval();

    expect("+a++ / 1").toEval();
    expect("+a-- / 1").toEval();
    expect("a.in / b").toEval();
    expect("a?.in.in / b").toEval();
    expect("a.instanceof / b").toEval();
    expect("a?.instanceof.instanceof / b").toEval();
    expect("class A { #name; d = a.#name / b; }").toEval();
    expect("class A { #name; d = a?.#name / b; }").toEval();

    expect("async / b").toEval();
    expect("a.delete / b").toEval();
    expect("a?.delete.delete / b").toEval();
    expect("delete / b/").toEval();
    expect("a.in / b").toEval();
    expect("a?.in.in / b").toEval();
    expect("for (a in / b/) {}").toEval();
    expect("a.instanceof / b").toEval();
    expect("a?.instanceof.instanceof / b").toEval();
    expect("a instanceof / b/").toEval();
    expect("a?.instanceof instanceof / b/").toEval();
    expect("new / b/").toEval();
    expect("null / b").toEval();
    expect("for (a of / b/) {}").toEval();
    expect("a.return / b").toEval();
    expect("a?.return.return / b").toEval();
    expect("function foo() { return / b/ }").toEval();
    expect("throw / b/").toEval();
    expect("a.typeof / b").toEval();
    expect("a?.typeof.typeof / b").toEval();
    expect("a.void / b").toEval();
    expect("a?.void.void / b").toEval();
    expect("void / b/").toEval();

    expect("await / b").toEval();
    expect("await / b/").not.toEval();
    expect("async function foo() { await / b }").not.toEval();
    expect("async function foo() { await / b/ }").toEval();

    expect("yield / b").toEval();
    expect("yield / b/").not.toEval();
    expect("function* foo() { yield / b }").not.toEval();
    expect("function* foo() { yield / b/ }").toEval();

    expect("this / 1").toEval();
    expect("this / 1 /").not.toEval();
    expect("this / 1 / 1").toEval();

    expect("this?.a / 1").toEval();
    expect("this?.a / 1 /").not.toEval();
    expect("this?.a / 1 / 1").toEval();
});
