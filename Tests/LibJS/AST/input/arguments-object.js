// Accessing `arguments` in a non-strict function.
function uses_arguments() {
    return arguments[0];
}

// Naming a parameter `arguments` shadows the arguments object.
function arguments_as_param(arguments) {
    return arguments;
}

// `arguments` as the second parameter gets [argument:1].
function arguments_as_second_param(x, arguments) {
    return x + arguments;
}

// `arguments` in an arrow function refers to the enclosing function.
function arrow_arguments() {
    let f = () => arguments[0];
    return f();
}

// Destructuring parameter still allows `arguments` access.
function destructured_with_arguments({ x }) {
    return arguments[0];
}

// Rest parameter + arguments.
function rest_and_arguments(...args) {
    return arguments.length;
}
