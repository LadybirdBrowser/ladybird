// Inner function captures a variable from the outer function.
// Both should mark their own locals correctly.
function outer() {
    let captured = 1;
    let not_captured = 2;
    function inner() {
        return captured;
    }
    return inner() + not_captured;
}

// Capture across two levels of nesting.
function level0() {
    let a = 1;
    function level1() {
        let b = 2;
        function level2() {
            return a + b;
        }
        return level2();
    }
    return level1();
}

// Closure created in a loop -- each iteration captures the same
// variable binding (with var) or different ones (with let).
function loop_closure_var() {
    var fns = [];
    for (var i = 0; i < 3; i++) {
        fns.push(function () {
            return i;
        });
    }
    return fns;
}

function loop_closure_let() {
    var fns = [];
    for (let i = 0; i < 3; i++) {
        fns.push(function () {
            return i;
        });
    }
    return fns;
}
