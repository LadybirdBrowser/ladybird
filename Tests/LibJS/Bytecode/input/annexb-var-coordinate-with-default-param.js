// An Annex B function var binding must be tracked in the variable environment,
// not in the parameter-expression environment.
function f(x = 1) {
    let y = 2;
    if (true) {
        function g() {
            return x + y;
        }
    }
    return g();
}
f();
