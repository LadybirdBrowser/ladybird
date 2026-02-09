// Comprehensive test of different binding forms and their local/global status.

// let, const, var at top level are all global.
let top_let = 1;
const top_const = 2;
var top_var = 3;

function all_binding_types() {
    // These should all be local.
    let a = 1;
    const b = 2;
    var c = 3;
    return a + b + c;
}

// for-loop heads create locals.
function for_binding_forms() {
    for (let i = 0; i < 1; i++) {}
    for (const x of [1]) {
        x;
    }
    for (var j = 0; j < 1; j++) {}
    j;
}

// Destructuring bindings in all forms.
function destructuring_bindings() {
    let [a, b] = [1, 2];
    const { c, d } = { c: 3, d: 4 };
    var [e, ...f] = [5, 6, 7];
    return a + b + c + d + e + f[0];
}

// Multiple declarators in one statement.
function multi_declarator() {
    let x = 1,
        y = 2,
        z = 3;
    return x + y + z;
}
