// Strict mode reserved words as identifiers in non-strict code.
function non_strict() {
    let x = 1;
    static_label: x = 2;
    return x;
}

// 'let' used as identifier in non-strict code.
function let_as_identifier() {
    var let = 42;
    return let;
}
