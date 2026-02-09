// Assignment targets: the LHS of assignments should still resolve
// as local or global correctly.
var g = 1;

function assign_local() {
    let x = 1;
    x = 2;
    x += 3;
    return x;
}

function assign_global() {
    g = 2;
    g += 3;
    return g;
}

// Destructuring assignment (not declaration).
function destruct_assign() {
    let a, b;
    [a, b] = [1, 2];
    return a + b;
}

// Assignment to a variable not declared anywhere (implicit global).
function assign_undeclared() {
    undeclared = 1;
    return undeclared;
}

// Increment/decrement targets.
function update_targets() {
    let x = 0;
    x++;
    ++x;
    x--;
    --x;
    return x;
}
