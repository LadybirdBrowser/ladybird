// Destructuring array pattern as LHS in for-in
function forin_array_pattern() {
    var a, b;
    for ([a, b] in { xy: 1 }) {}
}

// Destructuring object pattern as LHS in for-in
function forin_object_pattern() {
    var x, y;
    for ({ x, y } in { ab: 1 }) {}
}

// Destructuring array pattern as LHS in for-of
function forof_array_pattern() {
    var a, b;
    for ([a, b] of [[1, 2]]) {}
}

// Destructuring object pattern as LHS in for-of
function forof_object_pattern() {
    var x, y;
    for ({ x, y } of [{ x: 1, y: 2 }]) {}
}

// Member expression as LHS in for-in
function forin_member_lhs() {
    var obj = {};
    for (obj.key in { a: 1 }) {}
}

// Member expression as LHS in for-of
function forof_member_lhs() {
    var obj = {};
    for (obj.key of [1, 2]) {}
}
