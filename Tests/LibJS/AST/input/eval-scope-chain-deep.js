// eval in the middle of a deep nesting chain.
// Functions ABOVE the eval should keep their locals.
// The function WITH eval loses locals.
// Functions BELOW don't directly lose locals, but references to
// variables in the eval-poisoned scope become [in-eval-scope].
function clean_outer() {
    let a = 1;
    function middle() {
        let b = 2;
        eval("");
        function inner() {
            let c = 3;
            return a + b + c;
        }
        return inner();
    }
    return middle();
}

// eval at the top level of a nested chain.
function eval_at_top() {
    let x = 1;
    eval("");
    function level1() {
        let y = 2;
        function level2() {
            return x + y;
        }
        return level2();
    }
    return level1();
}
