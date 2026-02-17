function foo(x) {
    return x;
}
function bar(x) {
    return x;
}
function test() {
    var i = 0;
    foo([]);
    do {
        bar(i);
        i++;
    } while (i < 7);
}
test();
