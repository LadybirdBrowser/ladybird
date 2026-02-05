const sheet = document.head.appendChild(document.createElement("style"));

// Specify size for outer <div> to avoid unconstrained-size warnings
// when writing-mode of the inner test <div> is vertical-*
const wrapper = document.body.appendChild(document.createElement("div"));
wrapper.style.cssText = "width:100px; height: 100px;";
export const testElement = wrapper.appendChild(document.createElement("div"));
testElement.id = testElement.className = "test";

// Six unique overall writing modes for property-mapping purposes.
export const writingModes = [
    {
        styles: [
            { "writing-mode": "horizontal-tb", "direction": "ltr" },
        ],
        blockStart: "top", blockEnd: "bottom", inlineStart: "left", inlineEnd: "right",
        over: "top", under: "bottom", lineLeft: "left", lineRight: "right",
        block: "vertical", inline: "horizontal"
    },
    {
        styles: [
            { "writing-mode": "horizontal-tb", "direction": "rtl" },
        ],
        blockStart: "top", blockEnd: "bottom", inlineStart: "right", inlineEnd: "left",
        over: "top", under: "bottom", lineLeft: "left", lineRight: "right",
        block: "vertical", inline: "horizontal"
    },
    {
        styles: [
            { "writing-mode": "vertical-rl", "direction": "rtl" },
            { "writing-mode": "sideways-rl", "direction": "rtl" },
        ],
        blockStart: "right", blockEnd: "left", inlineStart: "bottom", inlineEnd: "top",
        over: "right", under: "left", lineLeft: "top", lineRight: "bottom",
        block: "horizontal", inline: "vertical"
    },
    {
        styles: [
            { "writing-mode": "vertical-rl", "direction": "ltr" },
            { "writing-mode": "sideways-rl", "direction": "ltr" },
        ],
        blockStart: "right", blockEnd: "left", inlineStart: "top", inlineEnd: "bottom",
        over: "right", under: "left", lineLeft: "top", lineRight: "bottom",
        block: "horizontal", inline: "vertical"
    },
    {
        styles: [
            { "writing-mode": "vertical-lr", "direction": "rtl" },
        ],
        blockStart: "left", blockEnd: "right", inlineStart: "bottom", inlineEnd: "top",
        over: "right", under: "left", lineLeft: "top", lineRight: "bottom",
        block: "horizontal", inline: "vertical"
    },
    {
        styles: [
            { "writing-mode": "sideways-lr", "direction": "ltr" },
        ],
        blockStart: "left", blockEnd: "right", inlineStart: "bottom", inlineEnd: "top",
        over: "left", under: "right", lineLeft: "bottom", lineRight: "top",
        block: "horizontal", inline: "vertical"
    },
    {
        styles: [
            { "writing-mode": "vertical-lr", "direction": "ltr" },
        ],
        blockStart: "left", blockEnd: "right", inlineStart: "top", inlineEnd: "bottom",
        over: "right", under: "left", lineLeft: "top", lineRight: "bottom",
        block: "horizontal", inline: "vertical"
    },
    {
        styles: [
            { "writing-mode": "sideways-lr", "direction": "rtl" },
        ],
        blockStart: "left", blockEnd: "right", inlineStart: "top", inlineEnd: "bottom",
        over: "left", under: "right", lineLeft: "bottom", lineRight: "top",
        block: "horizontal", inline: "vertical"
    },
];

// Check if logical properties work well in WebKit non-standard
// '-webkit-writing-mode: horizontal-bt' mode
if (CSS.supports("-webkit-writing-mode", "horizontal-bt")) {
    writingModes.push(
        {
            styles: [
                { "-webkit-writing-mode": "horizontal-bt", "direction": "ltr" },
            ],
            blockStart: "bottom", blockEnd: "top", inlineStart: "left", inlineEnd: "right",
            over: "top", under: "bottom", lineLeft: "left", lineRight: "right",
            block: "vertical", inline: "horizontal"
        },
        {
            styles: [
                { "-webkit-writing-mode": "horizontal-bt", "direction": "rtl" },
            ],
            blockStart: "bottom", blockEnd: "top", inlineStart: "right", inlineEnd: "left",
            over: "top", under: "bottom", lineLeft: "left", lineRight: "right",
            block: "vertical", inline: "horizontal"
        },
    )
}

export function testCSSValues(testName, style, expectedValues) {
    for (const [property, value] of expectedValues) {
        assert_equals(style.getPropertyValue(property), value, `${testName}, ${property}`);
    }
}

export function testComputedValues(testName, rules, expectedValues) {
    sheet.textContent = rules;
    const cs = getComputedStyle(testElement);
    testCSSValues(testName, cs, expectedValues);
    sheet.textContent = "";
}

export function makeDeclaration(object = {}, replacement = "*") {
    let decl = "";
    for (const [property, value] of Object.entries(object)) {
        decl += `${property.replace("*", replacement)}: ${value}; `;
    }
    return decl;
}
