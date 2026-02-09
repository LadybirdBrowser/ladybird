const anon = function () {
    let x = 1;
    return x;
};

const named = function myFunc() {
    return myFunc;
};

const recursive = function fib(n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
};
