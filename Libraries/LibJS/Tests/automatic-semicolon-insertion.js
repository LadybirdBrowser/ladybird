test("Issue #1829, if-else without braces or semicolons", () => {
    const source = `if (1)
    foo;
else
    bar;

if (1)
    foo
else
    bar

if (1)
    foo
else
    bar;`;

    expect(source).toEval();
});

test("break/continue, variable declaration, do-while, and return asi", () => {
    const source = `function foo() {
    label:
    for (var i = 0; i < 4; i++) {
        break // semicolon inserted here
        continue // semicolon inserted here

        break label // semicolon inserted here
        continue label // semicolon inserted here
    }

    var j // semicolon inserted here

    do {
    } while (1 === 2) // semicolon inserted here

    return // semicolon inserted here
    1;
var curly/* semicolon inserted here */}

foo();`;

    expect(source).toEvalTo(undefined);
});

test("more break and continue asi", () => {
    const source = `let counter = 0;
let outer;

outer:
for (let i = 0; i < 5; ++i) {
    for (let j = 0; j < 5; ++j) {
        continue // semicolon inserted here
        outer // semicolon inserted here
    }
    counter++;
}

counter;`;

    expect(source).toEvalTo(5);
});

test("eof with no semicolon", () => {
    expect("var eof").toEval();
});
