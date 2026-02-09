function continueThroughFinally() {
    let result = 0;
    for (let i = 0; i < 3; i++) {
        try {
            if (i === 1) continue;
            result += i;
        } finally {
            result += 10;
        }
    }
    return result;
}
console.log(continueThroughFinally());

function breakThroughFinally() {
    let result = 0;
    for (let i = 0; i < 10; i++) {
        try {
            if (i === 2) break;
            result += i;
        } finally {
            result += 100;
        }
    }
    return result;
}
console.log(breakThroughFinally());
