function range(a, b, c) {
    if (a < b && b < c) return 1;
    return 0;
}

function inverted(a, b) {
    if (!(a < b || b === 0)) return 1;
    return 0;
}

function loop(n) {
    let i = 0;
    while (i < n && n !== 0) i++;
    return i;
}

function once(n) {
    let i = 0;
    do {
        i++;
    } while (i < n && n !== 0);
    return i;
}

function count(n, ok) {
    let total = 0;
    for (let i = 0; i < n && ok; i++) total++;
    return total;
}

range(1, 2, 3);
inverted(2, 1);
loop(2);
once(2);
count(2, true);
