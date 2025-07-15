'use strict';

// =================================
//
// Common timing parameter test data
//
// =================================


// ------------------------------
//  Delay values
// ------------------------------

const gBadDelayValues = [
  NaN, Infinity, -Infinity
];

// ------------------------------
//  Duration values
// ------------------------------

const gGoodDurationValues = [
  { specified: 123.45, computed: 123.45 },
  { specified: 'auto', computed: 0 },
  { specified: Infinity, computed: Infinity },
];

const gBadDurationValues = [
  -1, NaN, -Infinity, 'abc', '100'
];

// ------------------------------
//  iterationStart values
// ------------------------------

const gBadIterationStartValues = [
  -1, NaN, Infinity, -Infinity
];

// ------------------------------
//  iterations values
// ------------------------------

const gBadIterationsValues = [
  -1, -Infinity, NaN
];
