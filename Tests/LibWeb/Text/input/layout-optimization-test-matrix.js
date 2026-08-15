function matrixCases(axes) {
    let cases = [{}];
    for (const [name, values] of Object.entries(axes)) {
        const expanded = [];
        for (const existing of cases) {
            for (const value of values) expanded.push({ ...existing, [name]: value });
        }
        cases = expanded;
    }
    return cases;
}

function rounded(value) {
    return Math.round(value * 64) / 64;
}

function relativeRect(element, ancestor) {
    const rect = element.getBoundingClientRect();
    const ancestorRect = ancestor.getBoundingClientRect();
    return [
        rounded(rect.left - ancestorRect.left),
        rounded(rect.top - ancestorRect.top),
        rounded(rect.width),
        rounded(rect.height),
    ];
}

class LayoutTestMatrix {
    constructor(name) {
        this.name = name;
        this.caseCount = 0;
        this.failures = [];
    }

    run(label, callback) {
        ++this.caseCount;
        try {
            callback();
        } catch (error) {
            this.failures.push(`${label}: ${error}`);
        }
    }

    async runAsync(label, callback) {
        ++this.caseCount;
        try {
            await callback();
        } catch (error) {
            this.failures.push(`${label}: ${error}`);
        }
    }

    expect(label, actual, expected) {
        const actualJSON = JSON.stringify(actual);
        const expectedJSON = JSON.stringify(expected);
        if (actualJSON !== expectedJSON) throw new Error(`${label}: expected ${expectedJSON}, got ${actualJSON}`);
    }

    expectTrue(label, condition) {
        if (!condition) throw new Error(`${label}: expected true`);
    }

    print() {
        for (const failure of this.failures) println(`FAIL: ${failure}`);
        println(`${this.name} cases: ${this.caseCount}`);
        println(`${this.name} failures: ${this.failures.length}`);
    }
}
