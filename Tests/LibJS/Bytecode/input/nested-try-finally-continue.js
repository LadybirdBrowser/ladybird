function nestedTryFinallyContinue() {
    for (let i = 0; i < 3; i++) {
        try {
            try {
                if (i === 1) continue;
            } finally {
                console.log("inner", i);
            }
        } finally {
            console.log("outer", i);
        }
    }
}
nestedTryFinallyContinue();
