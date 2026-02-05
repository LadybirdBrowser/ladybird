// Any copyright is dedicated to the Public Domain.
// https://creativecommons.org/publicdomain/zero/1.0/

const gridChildHelperRow = "row";
const gridChildHelperCol = "col";

// Helper for building testcases for grid-template-* with a child div in
// multiple positions. Prop is expected to be one of gridChildHelperRow or
// gridChildHelperCol, to select testing grid rows or grid columns,
// respectively.
// The child div is found by the id of 'child'.
function GridChildHelper(prop, style){
  this.child = document.getElementById("child");
  this.style = style;
  this.prop = prop;
}

// Runs a test for computed values on the property the helper object was
// constructed with. The childStyle is used for choosing the grid row/column
// of the child div.
// expected is passed as-is to the computed value test.
// The child style is appended to the test name in such a way that different
// tests for the same parent style but different child style values will have
// different test names.
GridChildHelper.prototype.runTest = function(childStyle, expected) {
  'use strict';
  const childProps = {
    [gridChildHelperCol]:"gridColumn",
    [gridChildHelperRow]:"gridRow"
  };
  const childProp = childProps[this.prop];

  const parentProps = {
    [gridChildHelperCol]:"grid-template-columns",
    [gridChildHelperRow]:"grid-template-rows"
  };
  const parentProp = parentProps[this.prop];

  const oldChildStyle = this.child[childProp];
  this.child.style[childProp] = childStyle;

  test_computed_value(parentProp, this.style, expected, childProp + " = " + childStyle);

  this.child[childProp] = oldChildStyle;
}
