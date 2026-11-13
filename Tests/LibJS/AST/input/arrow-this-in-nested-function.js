// An arrow function takes `this` from the nearest function that is not an arrow function.
// Functions further out must not be marked as reading `this` through the environment.
function outer_of_function() {
    function inner() {
        return () => this;
    }
    return inner;
}

function outer_of_class() {
    class C {
        m() {
            return () => this;
        }
    }
    return C;
}

function outer_of_object_method() {
    return {
        m() {
            return () => this;
        }
    };
}

function outer_of_arrow() {
    return () => () => this;
}
