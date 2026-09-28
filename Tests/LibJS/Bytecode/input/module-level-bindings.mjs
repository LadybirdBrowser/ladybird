var variable = 1;
let lexical = 2;
const constant = 3;

function outer() {
    return function inner() {
        variable = lexical + constant;
        return declared();
    };
}

function declared() {
    return typeof lexical;
}

outer()();
