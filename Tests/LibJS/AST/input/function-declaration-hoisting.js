// Function declarations are hoisted to the top of their scope.
function uses_hoisted() {
    hoisted();
    function hoisted() {
        return 1;
    }
}

// Function declaration in a block (annex B semantics).
function block_function() {
    {
        function inner() {
            return 1;
        }
        inner();
    }
}

// Two function declarations with the same name: last one wins.
function duplicate_decls() {
    function dup() {
        return 1;
    }
    function dup() {
        return 2;
    }
    return dup();
}

// Function declaration vs var with same name.
function func_vs_var() {
    var x = 1;
    function x() {}
    return x;
}
