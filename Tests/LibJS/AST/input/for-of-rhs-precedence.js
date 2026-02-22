// For-of RHS must be AssignmentExpression, not a comma expression.
// "for (x of a)" should parse the RHS as a single AssignmentExpression.
function for_of_simple_rhs() {
    var x;
    for (x of [1, 2, 3]) {}
}

// For-in RHS is an Expression (allows commas).
function for_in_comma_rhs() {
    var x;
    for (x in 0, {}) {}
}
