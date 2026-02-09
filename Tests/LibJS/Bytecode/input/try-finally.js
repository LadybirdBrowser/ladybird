function basicTryFinally() {
    try {
        return 1;
    } finally {
        console.log("finally");
    }
}
basicTryFinally();

function breakThroughFinally() {
    for (let i = 0; i < 10; i++) {
        try {
            if (i === 5) break;
        } finally {
            console.log(i);
        }
    }
}
breakThroughFinally();

function nestedTryFinallyWithBreak() {
    outer: for (let i = 0; ; i++) {
        try {
            try {
                break outer;
            } finally {
                console.log("inner");
            }
        } finally {
            console.log("outer");
        }
    }
}
nestedTryFinallyWithBreak();
