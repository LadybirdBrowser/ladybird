try {
    try {
        try {
            throw new Error("handled");
        } finally {
        }
    } finally {
    }
} catch {}

debugger;
