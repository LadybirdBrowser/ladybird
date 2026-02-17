// For loop with expression init (should NOT be wrapped in ExpressionStatement)
function for_expression_init() {
    for (x = 0; x < 10; x++) {}
}

// For loop with variable declaration init
function for_declaration_init() {
    for (var i = 0; i < 10; i++) {}
}

// For loop with let declaration init
function for_let_init() {
    for (let i = 0; i < 10; i++) {}
}

// For loop with no init
function for_no_init() {
    for (; x < 10; x++) {}
}
