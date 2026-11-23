test("yield as the parameter of an async arrow function", () => {
    expect("(function* () { return async yield => 1; })").not.toEval();
    expect("(function* () { return async (yield) => 1; })").not.toEval();
    expect("(function* () { return async \\u0079ield => 1; })").not.toEval();
    expect("(function* () { return function () { return async yield => 1; }; })").toEval();
    expect("(function () { return async yield => 1; })").toEval();
    expect("'use strict'; (function () { return async yield => 1; })").not.toEval();
});
