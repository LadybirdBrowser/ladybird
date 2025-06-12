test("regular comments", () => {
    const source = `
var i = 0;
// i++;
/* i++; */
/*
i++;
*/
/**/ i++;
i;`;

    expect(source).toEvalTo(1);
});

test("html comments", () => {
    const source = `
var i = 0;
var j = 0;
<!-- i++; --> i++;
<!-- i++;
i++;
--> i++;
/**/ --> i++;
j --> i++;
i;`;
    expect(source).toEvalTo(2);
});

test("html comments directly after block comment", () => {
    expect("0 /* */-->i").not.toEval();
    expect(`0 /* 
     */-->i`).toEval();
    expect(`0 /* 
     */-->i
     'a'`).toEvalTo("a");
});

test("unterminated multi-line comment", () => {
    expect("/*").not.toEval();
    expect("/**").not.toEval();
    expect("/*/").not.toEval();
    expect("/* foo").not.toEval();
    expect("foo /*").not.toEval();
});

test("hashbang comments", () => {
    expect("#!").toEvalTo(undefined);
    expect("#!/bin/js").toEvalTo(undefined);
    expect("#!\n1").toEvalTo(1);
    expect(" #!").not.toEval();
    expect("\n#!").not.toEval();
    expect("#!\n#!").not.toEval();
});
