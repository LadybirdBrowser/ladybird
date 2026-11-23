test("yield as the parameter of an async arrow function", () => {
    expect("(function* () { return async yield => 1; })").not.toEval();
    expect("(function* () { return async (yield) => 1; })").not.toEval();
    expect("(function* () { return async \\u0079ield => 1; })").not.toEval();
    expect("(function* () { return function () { return async yield => 1; }; })").toEval();
    expect("(function () { return async yield => 1; })").toEval();
    expect("'use strict'; (function () { return async yield => 1; })").not.toEval();
});

test("async of in the head of a for statement", () => {
    expect("async function f(y) { for await (async of y); }").toEval();
    expect("async function f(y) { for await (x[async of => 1] of y); }").toEval();
    expect("async function f(y) { for await (x[(() => { async of => 1; })()] of y); }").toEval();
    expect("async function f(y) { for await ((async of => 1).x of y); }").toEval();
    expect("async function f(y) { for await (async of => 1 of y); }").not.toEval();
    expect("for (async of => 1; false; );").toEval();
    expect("for (async of []);").not.toEval();
});
