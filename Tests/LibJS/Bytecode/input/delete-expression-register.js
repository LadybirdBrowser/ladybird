function f() {
    var x = false;
    x = x || delete arguments[0];
    return x;
}
f(1);
