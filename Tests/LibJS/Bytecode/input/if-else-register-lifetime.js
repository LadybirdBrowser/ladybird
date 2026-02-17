function test(x) {
    var a = 1;
    for (var i = 0; i < 10; i++) {
        for (var j = 0; j < 10; j++) {
            if (j < 5) x[j] = x[i + j];
            else x[j] = parseInt(x[j - 3], 1);
        }
    }
    return parseInt(a);
}
test([1]);
