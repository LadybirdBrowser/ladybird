// Direct eval inside an arrow function poisons the enclosing function too,
// because arrows don't have their own scope for var/this/arguments.
function outer() {
    let x = 1;
    let f = () => {
        eval("");
        return x;
    };
    return f();
}

// Arrow inside arrow inside regular function.
function deep_arrow() {
    let a = 1;
    let f = () => {
        let b = 2;
        let g = () => {
            eval("");
            return a + b;
        };
        return g();
    };
    return f();
}

// eval in arrow doesn't poison sibling functions.
function sibling() {
    let s = 1;
    let poison = () => eval("");
    function clean() {
        return s;
    }
    return clean();
}
