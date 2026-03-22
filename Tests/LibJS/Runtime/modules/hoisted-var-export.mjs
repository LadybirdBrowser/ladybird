// var declarations are hoisted to module scope even when nested,
// so they should be valid export targets.

for (var forVar = 1; false; ) {}
for (var forInVar in {}) {
}
for (var forOfVar of []) {
}
if (true) {
    var ifVar = 2;
}
{
    {
        {
            var blockVar = 3;
        }
    }
}
while (false) {
    var whileVar = 4;
}
do {
    var doWhileVar = 5;
} while (false);
try {
    var tryVar = 6;
} catch (e) {
    var catchVar = 7;
} finally {
    var finallyVar = 8;
}
switch (0) {
    case 0:
        var switchVar = 9;
        break;
}
label: {
    var labelVar = 10;
}
for (var multiA = 11, multiB = 12, multiC = 13; false; ) {}

export {
    forVar,
    forInVar,
    forOfVar,
    ifVar,
    blockVar,
    whileVar,
    doWhileVar,
    tryVar,
    catchVar,
    finallyVar,
    switchVar,
    labelVar,
    multiA,
    multiB,
    multiC,
};

export const passed = true;
