// Same source-order check, but for FunctionDeclarations nested inside a
// function body. The scope_collector builds functions_to_initialize for
// these and previously emitted them in reverse source order.

function outer() {
    function alpha() { return 1; }
    function beta() { return 2; }
    function dup() { return "first"; }
    function gamma() { return 3; }
    function dup() { return "second"; }
    function delta() { return 4; }
    return [alpha(), beta(), gamma(), delta(), dup()];
}

console.log(outer().join(","));
