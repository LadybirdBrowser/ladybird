// For-of RHS must be AssignmentExpression, not a comma expression.
function for_of_simple_rhs() {
    var x;
    for (x of [1, 2, 3]) {}
}

// For-in RHS allows comma expressions.
function for_in_comma_rhs() {
    var x;
    for (x in 0, {}) {}
}
