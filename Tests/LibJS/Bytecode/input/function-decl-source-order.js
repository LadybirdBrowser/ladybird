// Multiple top-level function declarations should be emitted in source
// order. ECMAScript "last function with name X wins" hoisting still
// applies, so the duplicate `dup` keeps the second body.

function alpha() { return 1; }

function beta() { return 2; }

function dup() { return "first"; }

function gamma() { return 3; }

function dup() { return "second"; }

function delta() { return 4; }

console.log(alpha(), beta(), gamma(), delta(), dup());
