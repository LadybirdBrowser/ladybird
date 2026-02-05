var flexDirections = ["row", "row-reverse", "column", "column-reverse"];
var textDirections = ["ltr", "rtl"];
var writingModes = ["horizontal", "flipped-blocks", "flipped-lines"];

var createLeafNode = (i) => {
  var flexItem = document.createElement("div");
  flexItem.className = "leaf" + i;
  var contentItem = document.createElement("div");
  contentItem.innerHTML = i;
  flexItem.appendChild(contentItem);
  return flexItem;
}

var createContentNode = (flexDirection, textDirection, writingMode) => {
  var flexNode = document.createElement("div");
  flexNode.className = "flex " + flexDirection;
  flexNode.title = "flex-direction: " + flexDirection + "; direction: " + textDirection + "; writing-mode: " + writingMode;
  for (var i = 1; i < 4; i++)
    flexNode.appendChild(createLeafNode(i));
  var marginShim = document.createElement("div");
  return flexNode;
}

var createContainerNode = (flexDirection, textDirection, writingMode) => {
  var containerNode = document.createElement("div");
  containerNode.className = "container " + textDirection + " " + writingMode;
  containerNode.appendChild(createContentNode(flexDirection, textDirection, writingMode));
  return containerNode;
}

var createContainerRow = (flexDirection) => {
  var containerRow = document.createElement("div");
  containerRow.className = "container-row";
  var rowLabel = document.createElement("div");
  rowLabel.className = "vertical-header horizontal " + flexDirection;
  rowLabel.innerHTML = flexDirection;
  containerRow.appendChild(rowLabel);
  textDirections.forEach((textDirection) => {
    writingModes.forEach((writingMode) => {
      var containerNode = createContainerNode(flexDirection, textDirection, writingMode);
      containerRow.appendChild(containerNode);
    });
  });
  return containerRow;
}
