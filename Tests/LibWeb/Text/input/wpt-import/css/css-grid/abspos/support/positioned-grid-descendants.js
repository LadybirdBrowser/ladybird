// Track sizes, including preceding padding and following remaining space
const colSizes = [5, 200, 300, 65];
const rowSizes = [20, 150, 100, 160];

// Calculate track positions
const colOffsets = [0];
const rowOffsets = [0];
for (const size of colSizes)
  colOffsets.push(size + colOffsets[colOffsets.length - 1]);
for (const size of rowSizes)
  rowOffsets.push(size + rowOffsets[rowOffsets.length - 1]);

export function runTests({left, top, right, bottom, orthogonal = false}) {
  // Iterate all pairs of grid lines, where 0 and 4 represent "auto".
  for (let colStart = 0; colStart < 4; ++colStart)
  for (let colEnd = colStart + 1; colEnd <= 4; ++colEnd)
  for (let rowStart = 0; rowStart < 4; ++rowStart)
  for (let rowEnd = rowStart + 1; rowEnd <= 4; ++rowEnd)
  {
    // Create a 2x2 grid with two grid items, each one containing an abspos.
    const grid = document.createElement("div");
    grid.className = "grid";
    for (let i = 1; i <= 2; ++i) {
      // Create a grid item with some content before the abspos, so that the static
      // position is shifted 50px to the right and 25px to the bottom.
      const gridItem = document.createElement("div");
      gridItem.style.gridArea = `${i} / ${i}`;
      grid.appendChild(gridItem);
      gridItem.innerHTML = "X<br />XX";

      // Create an abspos with content of 50px inline size, 25px block size.
      const absPos = document.createElement("div");
      gridItem.appendChild(absPos);
      absPos.className = "abspos";
      absPos.textContent = "XX";
      if (orthogonal) absPos.classList.add("orthogonal");

      // Let the containing block be the grid area, and set offsets.
      Object.assign(absPos.style, {
        gridColumnStart: colStart || "auto",
        gridColumnEnd: colEnd % 4 || "auto",
        gridRowStart: rowStart || "auto",
        gridRowEnd: rowEnd % 4 || "auto",
        left: left == "auto" ? left : left + "px",
        top: top == "auto" ? top : top + "px",
        right: right == "auto" ? right : right + "px",
        bottom: bottom == "auto" ? bottom : bottom + "px",
      });

      // Calculate expected position and size.
      const expectedWidth =
        left == "auto" || right == "auto" ? 25 * (orthogonal ? 1 : 2) :
        Math.max(0, colOffsets[colEnd] - colOffsets[colStart] - left - right);
      const expectedHeight =
        top == "auto" || bottom == "auto" ? 25 * (orthogonal ? 2 : 1) :
        Math.max(0, rowOffsets[rowEnd] - rowOffsets[rowStart] - top - bottom);
      const offsetX =
        left != "auto" ? colOffsets[colStart] + left :
        right != "auto" ? colOffsets[colEnd] - right - expectedWidth :
        colOffsets[i] + 25*2;
      const offsetY =
        top != "auto" ? rowOffsets[rowStart] + top :
        bottom != "auto" ? rowOffsets[rowEnd] - bottom - expectedHeight :
        rowOffsets[i] + 25;
      Object.assign(absPos.dataset, {expectedWidth, expectedHeight, offsetX, offsetY});
    }
    document.body.appendChild(grid);
  }

  document.fonts.ready.then(() => {
    checkLayout(".grid");
  });
}
