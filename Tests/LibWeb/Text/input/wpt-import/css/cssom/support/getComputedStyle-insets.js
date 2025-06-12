export const testEl = document.createElement("div");
export const containerForInflow = document.createElement("div");
export const containerForAbspos = document.createElement("div");
export const containerForFixed = document.createElement("div");

testEl.id = "test";
containerForInflow.id = "container-for-inflow";
containerForAbspos.id = "container-for-abspos";
containerForFixed.id = "container-for-fixed";

containerForInflow.appendChild(testEl);
containerForAbspos.appendChild(containerForInflow);
containerForFixed.appendChild(containerForAbspos);
document.body.appendChild(containerForFixed);

const stylesheet = document.createElement("style");
stylesheet.textContent = `
  #container-for-inflow {
    /* Content area: 100px tall, 200px wide */
    height: 100px;
    width: 200px;
    padding: 1px 2px;
    border-width: 2px 4px;
    margin: 4px 8px;
  }
  #container-for-abspos {
    /* Padding area: 200px tall, 400px wide */
    height: 184px;
    width: 368px;
    padding: 8px 16px;
    border-width: 16px 32px;
    margin: 32px 64px;
    position: relative;
  }
  #container-for-fixed {
    /* Padding area: 300px tall, 600px wide */
    height: 172px;
    width: 344px;
    padding: 64px 128px;
    border-width: 128px 256px;
    margin: 256px 512px;
    position: absolute;
    transform: scale(1);
    visibility: hidden;
  }
  [id ^= container] {
    border-style: solid;
  }
`;
document.head.prepend(stylesheet);

function runTestsWithWM(data, testWM, cbWM) {
  const {
    style,
    containingBlockElement,
    containingBlockArea,
    preservesPercentages,
    preservesAuto,
    canStretchAutoSize,
    staticPositionX,
    staticPositionY,
  } = data;

  let cbHeight = containingBlockElement ? containingBlockElement.clientHeight : NaN;
  let cbWidth = containingBlockElement ? containingBlockElement.clientWidth : NaN;
  if (containingBlockElement && containingBlockArea == "content") {
    const cs = getComputedStyle(containingBlockElement);
    cbHeight -= parseFloat(cs.paddingTop) + parseFloat(cs.paddingBottom);
    cbWidth -= parseFloat(cs.paddingLeft) + parseFloat(cs.paddingRight);
  }

  const staticPositionTop = cbWM.blockStart == "top" || cbWM.inlineStart == "top"
    ? staticPositionY : cbHeight - staticPositionY;
  const staticPositionLeft = cbWM.blockStart == "left" || cbWM.inlineStart == "left"
    ? staticPositionX : cbWidth - staticPositionX;
  const staticPositionBottom = cbWM.blockStart == "bottom" || cbWM.inlineStart == "bottom"
    ? staticPositionY : cbHeight - staticPositionY;
  const staticPositionRight = cbWM.blockStart == "right" || cbWM.inlineStart == "right"
    ? staticPositionX : cbWidth - staticPositionX;

  function serialize(declarations) {
    return Object.entries(declarations).map(([p, v]) => `${p}: ${v}; `).join("");
  }

  function wmName(wm) {
    return Object.values(wm.style).join(" ");
  }

  function checkStyle(declarations, expected, msg) {
    test(function() {
      testEl.style.cssText = style + "; " + serialize(Object.assign({}, declarations, testWM.style));
      if (containingBlockElement) {
        containingBlockElement.style.cssText = serialize(Object.assign({}, cbWM.style));
      }
      const cs = getComputedStyle(testEl);
      for (let [prop, value] of Object.entries(expected)) {
        assert_equals(cs[prop], value, `'${prop}'`);
      }
    }, `${wmName(testWM)} inside ${wmName(cbWM)} - ${msg}`);

    testEl.style.cssText = "";
    if (containingBlockElement) {
      containingBlockElement.style.cssText = "";
    }
  }

  checkStyle({
    top: "1px",
    left: "2px",
    bottom: "3px",
    right: "4px",
  }, {
    top: "1px",
    left: "2px",
    bottom: "3px",
    right: "4px",
  }, "Pixels resolve as-is");

  checkStyle({
    top: "1em",
    left: "2em",
    bottom: "3em",
    right: "4em",
    "font-size": "10px",
  }, {
    top: "10px",
    left: "20px",
    bottom: "30px",
    right: "40px",
  }, "Relative lengths are absolutized into pixels");

  if (preservesPercentages) {
    checkStyle({
      top: "10%",
      left: "25%",
      bottom: "50%",
      right: "75%",
    }, {
      top: "10%",
      left: "25%",
      bottom: "50%",
      right: "75%",
    }, "Percentages resolve as-is");
  } else {
    checkStyle({
      top: "10%",
      left: "25%",
      bottom: "50%",
      right: "75%",
    }, {
      top: cbHeight * 10 / 100 + "px",
      left: cbWidth * 25 / 100 + "px",
      bottom: cbHeight * 50 / 100 + "px",
      right: cbWidth * 75 / 100 + "px",
    }, "Percentages are absolutized into pixels");

    checkStyle({
      top: "calc(10% - 1px)",
      left: "calc(25% - 2px)",
      bottom: "calc(50% - 3px)",
      right: "calc(75% - 4px)",
    }, {
      top: cbHeight * 10 / 100 - 1 + "px",
      left: cbWidth * 25 / 100 - 2 + "px",
      bottom: cbHeight * 50 / 100 - 3 + "px",
      right: cbWidth * 75 / 100 - 4 + "px",
    }, "calc() is absolutized into pixels");
  }

  if (canStretchAutoSize) {
    // Force overconstraintment by setting size or with insets that would result in
    // negative size. Then the resolved value should be the computed one according to
    // https://drafts.csswg.org/cssom/#resolved-value-special-case-property-like-top

    checkStyle({
      top: "1px",
      left: "2px",
      bottom: "3px",
      right: "4px",
      height: "0px",
      width: "0px",
    }, {
      top: "1px",
      left: "2px",
      bottom: "3px",
      right: "4px",
    }, "Pixels resolve as-is when overconstrained");

    checkStyle({
      top: "100%",
      left: "100%",
      bottom: "100%",
      right: "100%",
    }, {
      top: cbHeight + "px",
      left: cbWidth + "px",
      bottom: cbHeight + "px",
      right: cbWidth + "px",
    }, "Percentages absolutize the computed value when overconstrained");
  }

  if (preservesAuto) {
    checkStyle({
      top: "auto",
      left: "auto",
      bottom: "3px",
      right: "4px",
    }, {
      top: "auto",
      left: "auto",
      bottom: "3px",
      right: "4px",
    }, "If start side is 'auto' and end side is not, 'auto' resolves as-is");

    checkStyle({
      top: "1px",
      left: "2px",
      bottom: "auto",
      right: "auto",
    }, {
      top: "1px",
      left: "2px",
      bottom: "auto",
      right: "auto",
    }, "If end side is 'auto' and start side is not, 'auto' resolves as-is");

    checkStyle({
      top: "auto",
      left: "auto",
      bottom: "auto",
      right: "auto",
    }, {
      top: "auto",
      left: "auto",
      bottom: "auto",
      right: "auto",
    }, "If opposite sides are 'auto', they resolve as-is");
  } else if (canStretchAutoSize) {
    checkStyle({
      top: "auto",
      left: "auto",
      bottom: "3px",
      right: "4px",
    }, {
      top: cbHeight - 3 + "px",
      left: cbWidth - 4 + "px",
      bottom: "3px",
      right: "4px",
    }, "If start side is 'auto' and end side is not, 'auto' resolves to used value");

    checkStyle({
      top: "1px",
      left: "2px",
      bottom: "auto",
      right: "auto",
    }, {
      top: "1px",
      left: "2px",
      bottom: cbHeight - 1 + "px",
      right: cbWidth - 2 + "px",
    }, "If end side is 'auto' and start side is not, 'auto' resolves to used value");

    checkStyle({
      top: "auto",
      left: "auto",
      bottom: "auto",
      right: "auto",
    }, {
      top: staticPositionTop + "px",
      left: staticPositionLeft + "px",
      bottom: staticPositionBottom + "px",
      right: staticPositionRight + "px",
    }, "If opposite sides are 'auto', they resolve to used value");
  } else {
    checkStyle({
      top: "auto",
      left: "auto",
      bottom: "3px",
      right: "4px",
    }, {
      top: "-3px",
      left: "-4px",
      bottom: "3px",
      right: "4px",
    }, "If start side is 'auto' and end side is not, 'auto' resolves to used value");

    checkStyle({
      top: "1px",
      left: "2px",
      bottom: "auto",
      right: "auto",
    }, {
      top: "1px",
      left: "2px",
      bottom: "-1px",
      right: "-2px",
    }, "If end side is 'auto' and start side is not, 'auto' resolves to used value");

    checkStyle({
      top: "auto",
      left: "auto",
      bottom: "auto",
      right: "auto",
    }, {
      top: "0px",
      left: "0px",
      bottom: "0px",
      right: "0px",
    }, "If opposite sides are 'auto', they resolve to used value");
  }
}

const writingModes = [{
  style: {
    "writing-mode": "horizontal-tb",
    "direction": "ltr",
  },
  blockStart: "top",
  blockEnd: "bottom",
  inlineStart: "left",
  inlineEnd: "right",
}, {
  style: {
    "writing-mode": "horizontal-tb",
    "direction": "rtl",
  },
  blockStart: "top",
  blockEnd: "bottom",
  inlineStart: "right",
  inlineEnd: "left",
}, {
  style: {
    "writing-mode": "vertical-lr",
    "direction": "ltr",
  },
  blockStart: "left",
  blockEnd: "right",
  inlineStart: "top",
  inlineEnd: "bottom",
}, {
  style: {
    "writing-mode": "vertical-lr",
    "direction": "rtl",
  },
  blockStart: "left",
  blockEnd: "right",
  inlineStart: "bottom",
  inlineEnd: "top",
}, {
  style: {
    "writing-mode": "vertical-rl",
    "direction": "ltr",
  },
  blockStart: "right",
  blockEnd: "left",
  inlineStart: "top",
  inlineEnd: "bottom",
}, {
  style: {
    "writing-mode": "vertical-rl",
    "direction": "rtl",
  },
  blockStart: "right",
  blockEnd: "left",
  inlineStart: "bottom",
  inlineEnd: "top",
}];

export function runTests(data) {
  for (let testWM of writingModes) {
    for (let cbWM of writingModes) {
      runTestsWithWM(data, testWM, cbWM);
    }
  }
}
