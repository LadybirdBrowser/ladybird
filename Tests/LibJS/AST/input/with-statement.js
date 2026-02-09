function with_basic(obj) {
    with (obj) {
        return x;
    }
}

function with_and_local(obj) {
    let y = 1;
    with (obj) {
        return x + y;
    }
}

function with_nested_function(obj) {
    with (obj) {
        function inner() {
            let z = 2;
            return z;
        }
        return inner();
    }
}
