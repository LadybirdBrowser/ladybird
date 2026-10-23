// Calls of bound functions with more bound arguments than the old limit of four are inlined, and calls of bound
// functions with more bound arguments than the inlining limit are not: both must see every bound argument.
function sum(...values) {
    let total = 0;
    for (const value of values) total += value;
    return total + values.length * 1000;
}
const count = n => Array.from({ length: n }, (_, index) => index + 1);
for (const n of [5, 16, 64, 65, 200]) {
    const bound = sum.bind(null, ...count(n));
    // NB: Each caller has its own executable, so its call site sees only one bound function.
    const call = Function("bound", "return argument => bound(argument);")(bound);
    let result = 0;
    for (let k = 0; k < 200; k++) result = call(k);
    print(n, result);
}
