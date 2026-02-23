// When the parser sees `(a = this)` it speculatively tries arrow function
// parsing. During that attempt, `this` inside the default value propagates
// uses_this_from_environment to ancestor function scopes. When the arrow
// attempt fails (no => follows), these flags must NOT be cleaned up --
// matching C++ behavior where save/restore of these flags is not implemented.
function outer() {
    var f = function(a) {
        (a = this);
        return a;
    };
    return f;
}
