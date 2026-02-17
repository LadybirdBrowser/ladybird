function foo(x) {
    return x;
}
function test(c) {
    c[3]++;
    foo(c);
}
test([1, 2, 3, 4]);
