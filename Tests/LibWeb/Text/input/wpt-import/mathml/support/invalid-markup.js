// This is a helper for generating invalid MathML markup.
// Depends on ./mathml-fragments.js

function isValid(tagName, mspaceCount) {
  switch (tagName) {
    case "mfrac":
    case "mroot":
    case "munder":
    case "mover":
    case "msub":
    case "msup":
      return mspaceCount == 2;
    case "munderover":
    case "msubsup":
      return mspaceCount == 3;
    case "mmultiscripts":
      return mspaceCount % 2 == 1;
  }
}

function generateInvalidMarkup() {
  let container = document.createElement("div");
  ["mfrac", "mroot", "munder", "mover", "munderover", "msub", "msup", "msubsup", "mmultiscripts"].forEach(tag => {
    let math = FragmentHelper.createElement("math");
    let element = FragmentHelper.createElement(tag);
    let reference = FragmentHelper.createElement("mrow");
    math.appendChild(element);
    math.appendChild(reference);
    let maxCount = tag == "mmultiscripts" ? 10 : 5;
    let mspaceCount = 0;
    for (let count = 0; count <= maxCount; count++) {
      element.dataset.description = `count == ${count}`;
      if (!isValid(tag, mspaceCount)) {
        container.appendChild(math.cloneNode(true));
      }
      if (tag == "mmultiscripts" && count == maxCount / 2) {
        [element, reference].forEach(el => {
          el.insertAdjacentHTML("beforeend", `<mprescripts/>`);
        });
      } else {
        let width = (count + 1) * 10;
        let height = (count + 1) * (count % 2 ? 15 : 5);
        let depth = (count + 1) * (count % 2 ? 5 : 15);
        [element, reference].forEach(el => {
          el.insertAdjacentHTML("beforeend", `<mspace height="${height}px" depth="${depth}px" width="${width}px" style="background: black"/>`);
        });
        mspaceCount++;
      }
    }
  });

  container.insertAdjacentHTML("beforeend", `
<math>
  <mmultiscripts data-description="first in-flow child is an <mprescripts>">
    <mprescripts/>
    <mspace height="5px" depth="15px" width="10px" style="background: black"/>
    <mspace height="30px" depth="10px" width="20px" style="background: black"/>
    <mspace height="15px" depth="45px" width="30px" style="background: black"/>
    <mspace height="60px" depth="20px" width="40px" style="background: black"/>

  </mmultiscripts>
  <mrow>
    <mprescripts/>
    <mspace height="5px" depth="15px" width="10px" style="background: black"/>
    <mspace height="30px" depth="10px" width="20px" style="background: black"/>
    <mspace height="15px" depth="45px" width="30px" style="background: black"/>
    <mspace height="60px" depth="20px" width="40px" style="background: black"/>

  </mrow>
</math>
<math>
  <mmultiscripts data-description="one of the even number of children after the first <mprescripts> is an <mprescripts>">
    <mspace height="5px" depth="15px" width="10px" style="background: black"/>
    <mspace height="30px" depth="10px" width="20px" style="background: black"/>
    <mspace height="15px" depth="45px" width="30px" style="background: black"/>
    <mprescripts/>
    <mspace height="60px" depth="20px" width="40px" style="background: black"/>
    <mprescripts/>
    <mspace height="25px" depth="75px" width="50px" style="background: black"/>
    <mspace height="35px" depth="105px" width="70px" style="background: black"/>
  </mmultiscripts>
  <mrow>
    <mspace height="5px" depth="15px" width="10px" style="background: black"/>
    <mspace height="30px" depth="10px" width="20px" style="background: black"/>
    <mspace height="15px" depth="45px" width="30px" style="background: black"/>
    <mprescripts/>
    <mspace height="60px" depth="20px" width="40px" style="background: black"/>
    <mprescripts/>
    <mspace height="25px" depth="75px" width="50px" style="background: black"/>
    <mspace height="35px" depth="105px" width="70px" style="background: black"/>
  </mrow>
</math>
  `);

  return container;
}
