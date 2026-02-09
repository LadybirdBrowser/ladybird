function control(x) {
    if (x > 0) {
        return x;
    } else {
        return -x;
    }
}

function loops(n) {
    let sum = 0;
    for (let i = 0; i < n; i++) {
        sum += i;
    }
    while (sum > 100) {
        sum -= 10;
    }
    return sum;
}
