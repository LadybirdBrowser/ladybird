function f(a, b, c) {
    var inner = function () {
        return a + b + c;
    };
    return inner();
}
f(1, 2, 3);
