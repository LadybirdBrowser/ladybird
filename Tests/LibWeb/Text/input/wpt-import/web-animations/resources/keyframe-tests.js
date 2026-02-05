'use strict';

// ==============================
//
// Common keyframe test data
//
// ==============================


// ------------------------------
//  Composite values
// ------------------------------

const gGoodKeyframeCompositeValueTests = [
  'replace', 'add', 'accumulate', 'auto'
];

const gBadKeyframeCompositeValueTests = [
  'unrecognised', 'replace ', 'Replace', null
];

const gGoodOptionsCompositeValueTests = [
  'replace', 'add', 'accumulate'
];

const gBadOptionsCompositeValueTests = [
  'unrecognised', 'replace ', 'Replace', null
];

// ------------------------------
//  Keyframes
// ------------------------------

const gEmptyKeyframeListTests = [
  [],
  null,
  undefined,
];

// Helper methods to make defining computed keyframes more readable.

const offset = offset => ({
  offset,
  computedOffset: offset,
});

const computedOffset = computedOffset => ({
  offset: null,
  computedOffset,
});

const keyframe = (offset, props, easing='linear', composite) => {
  // The object spread operator is not yet available in all browsers so we use
  // Object.assign instead.
  const result = {};
  Object.assign(result, offset, props, { easing });
  result.composite = composite || 'auto';
  return result;
};

const gKeyframesTests = [

  // ----------- Property-indexed keyframes: property handling -----------

  {
    desc:   'a one property two value property-indexed keyframes specification',
    input:  { left: ['10px', '20px'] },
    output: [keyframe(computedOffset(0), { left: '10px' }),
             keyframe(computedOffset(1), { left: '20px' })],
  },
  {
    desc:   'a one shorthand property two value property-indexed keyframes'
            + ' specification',
    input:  { margin: ['10px', '10px 20px 30px 40px'] },
    output: [keyframe(computedOffset(0), { margin: '10px' }),
             keyframe(computedOffset(1), { margin: '10px 20px 30px 40px' })],
  },
  {
    desc:   'a two property (one shorthand and one of its longhand components)'
            + ' two value property-indexed keyframes specification',
    input:  { marginTop: ['50px', '60px'],
              margin: ['10px', '10px 20px 30px 40px'] },
    output: [keyframe(computedOffset(0),
                      { marginTop: '50px', margin: '10px' }),
             keyframe(computedOffset(1),
                      { marginTop: '60px', margin: '10px 20px 30px 40px' })],
  },
  {
    desc:   'a two property (one shorthand and one of its shorthand components)'
            + ' two value property-indexed keyframes specification',
    input:  { border: ['pink', '2px'],
              borderColor: ['green', 'blue'] },
    output: [keyframe(computedOffset(0),
                      { border: 'pink', borderColor: 'green' }),
             keyframe(computedOffset(1),
                      { border: '2px', borderColor: 'blue' })],
  },
  {
    desc:   'a two property two value property-indexed keyframes specification',
    input:  { left: ['10px', '20px'],
              top: ['30px', '40px'] },
    output: [keyframe(computedOffset(0), { left: '10px', top: '30px' }),
             keyframe(computedOffset(1), { left: '20px', top: '40px' })],
  },
  {
    desc:   'a two property property-indexed keyframes specification with'
            + ' different numbers of values',
    input:  { left: ['10px', '20px', '30px'],
              top: ['40px', '50px'] },
    output: [keyframe(computedOffset(0),   { left: '10px', top: '40px' }),
             keyframe(computedOffset(0.5), { left: '20px' }),
             keyframe(computedOffset(1),   { left: '30px', top: '50px' })],
  },
  {
    desc:   'a property-indexed keyframes specification with an invalid value',
    input:  { left: ['10px', '20px', '30px', '40px', '50px'],
              top:  ['15px', '25px', 'invalid', '45px', '55px'] },
    output: [keyframe(computedOffset(0),    { left: '10px', top: '15px' }),
             keyframe(computedOffset(0.25), { left: '20px', top: '25px' }),
             keyframe(computedOffset(0.5),  { left: '30px' }),
             keyframe(computedOffset(0.75), { left: '40px', top: '45px' }),
             keyframe(computedOffset(1),    { left: '50px', top: '55px' })],
  },
  {
    desc:   'a one property two value property-indexed keyframes specification'
            + ' that needs to stringify its values',
    input:  { opacity: [0, 1] },
    output: [keyframe(computedOffset(0), { opacity: '0' }),
             keyframe(computedOffset(1), { opacity: '1' })],
  },
  {
    desc:   'a property-indexed keyframes specification with a CSS variable'
            + ' reference',
    input:  { left: [ 'var(--dist)', 'calc(var(--dist) + 100px)' ] },
    output: [keyframe(computedOffset(0), { left: 'var(--dist)' }),
             keyframe(computedOffset(1), { left: 'calc(var(--dist) + 100px)' })]
  },
  {
    desc:   'a property-indexed keyframes specification with a CSS variable'
            + ' reference in a shorthand property',
    input:  { margin: [ 'var(--dist)', 'calc(var(--dist) + 100px)' ] },
    output: [keyframe(computedOffset(0),
                      { margin: 'var(--dist)' }),
             keyframe(computedOffset(1),
                      { margin: 'calc(var(--dist) + 100px)' })],
  },
  {
    desc:   'a one property one value property-indexed keyframes specification',
    input:  { left: ['10px'] },
    output: [keyframe(computedOffset(1), { left: '10px' })],
  },
  {
    desc:   'a one property one non-array value property-indexed keyframes'
            + ' specification',
    input:  { left: '10px' },
    output: [keyframe(computedOffset(1), { left: '10px' })],
  },
  {
    desc:   'a one property two value property-indexed keyframes specification'
            + ' where the first value is invalid',
    input:  { left: ['invalid', '10px'] },
    output: [keyframe(computedOffset(0), {}),
             keyframe(computedOffset(1), { left: '10px' })]
  },
  {
    desc:   'a one property two value property-indexed keyframes specification'
            + ' where the second value is invalid',
    input:  { left: ['10px', 'invalid'] },
    output: [keyframe(computedOffset(0), { left: '10px' }),
             keyframe(computedOffset(1), {})]
  },
  {
    desc:   'a property-indexed keyframes specification with a CSS variable as'
            + ' the property',
    input:  { '--custom': ['1', '2'] },
    output: [keyframe(computedOffset(0), { '--custom': '1' }),
             keyframe(computedOffset(1), { '--custom': '2' })]
  },

  // ----------- Property-indexed keyframes: offset handling -----------

  {
    desc:   'a property-indexed keyframe with a single offset',
    input:  { left: ['10px', '20px', '30px'], offset: 0.5 },
    output: [keyframe(offset(0.5),          { left: '10px' }),
             keyframe(computedOffset(0.75), { left: '20px' }),
             keyframe(computedOffset(1),    { left: '30px' })],
  },
  {
    desc:   'a property-indexed keyframe with an array of offsets',
    input:  { left: ['10px', '20px', '30px'], offset: [ 0.1, 0.25, 0.8 ] },
    output: [keyframe(offset(0.1),  { left: '10px' }),
             keyframe(offset(0.25), { left: '20px' }),
             keyframe(offset(0.8),  { left: '30px' })],
  },
  {
    desc:   'a property-indexed keyframe with an array of offsets that is too'
            + ' short',
    input:  { left: ['10px', '20px', '30px'], offset: [ 0, 0.25 ] },
    output: [keyframe(offset(0),         { left: '10px' }),
             keyframe(offset(0.25),      { left: '20px' }),
             keyframe(computedOffset(1), { left: '30px' })],
  },
  {
    desc:   'a property-indexed keyframe with an array of offsets that is too'
            + ' long',
    input:  { left: ['10px', '20px', '30px'],
              offset: [ 0, 0.25, 0.5, 0.75, 1 ] },
    output: [keyframe(offset(0),    { left: '10px' }),
             keyframe(offset(0.25), { left: '20px' }),
             keyframe(offset(0.5),  { left: '30px' })],
  },
  {
    desc:   'a property-indexed keyframe with an empty array of offsets',
    input:  { left: ['10px', '20px', '30px'], offset: [] },
    output: [keyframe(computedOffset(0),   { left: '10px' }),
             keyframe(computedOffset(0.5), { left: '20px' }),
             keyframe(computedOffset(1),   { left: '30px' })],
  },
  {
    desc:   'a property-indexed keyframe with an array of offsets with an'
            + ' embedded null value',
    input:  { left: ['10px', '20px', '30px'],
              offset: [ 0, null, 0.5 ] },
    output: [keyframe(offset(0),            { left: '10px' }),
             keyframe(computedOffset(0.25), { left: '20px' }),
             keyframe(offset(0.5),          { left: '30px' })],
  },
  {
    desc:   'a property-indexed keyframe with an array of offsets with a'
            + ' trailing null value',
    input:  { left: ['10px', '20px', '30px'],
              offset: [ 0, 0.25, null ] },
    output: [keyframe(offset(0),           { left: '10px' }),
             keyframe(offset(0.25),        { left: '20px' }),
             keyframe(computedOffset(1), { left: '30px' })],
  },
  {
    desc:   'a property-indexed keyframe with an array of offsets with leading'
            + ' and trailing null values',
    input:  { left: ['10px', '20px', '30px'],
              offset: [ null, 0.25, null ] },
    output: [keyframe(computedOffset(0), { left: '10px' }),
             keyframe(offset(0.25),      { left: '20px' }),
             keyframe(computedOffset(1), { left: '30px' })],
  },
  {
    desc:   'a property-indexed keyframe with an array of offsets with'
            + ' adjacent null values',
    input:  { left: ['10px', '20px', '30px'],
              offset: [ null, null, 0.5 ] },
    output: [keyframe(computedOffset(0),    { left: '10px' }),
             keyframe(computedOffset(0.25), { left: '20px' }),
             keyframe(offset(0.5),          { left: '30px' })],
  },
  {
    desc:   'a property-indexed keyframe with an array of offsets with'
            + ' all null values (and too many at that)',
    input:  { left: ['10px', '20px', '30px'],
              offset: [ null, null, null, null, null ] },
    output: [keyframe(computedOffset(0),   { left: '10px' }),
             keyframe(computedOffset(0.5), { left: '20px' }),
             keyframe(computedOffset(1),   { left: '30px' })],
  },
  {
    desc:   'a property-indexed keyframe with a single null offset',
    input:  { left: ['10px', '20px', '30px'], offset: null },
    output: [keyframe(computedOffset(0),   { left: '10px' }),
             keyframe(computedOffset(0.5), { left: '20px' }),
             keyframe(computedOffset(1),   { left: '30px' })],
  },
  {
    desc:   'a property-indexed keyframe with an array of offsets that is not'
            + ' strictly ascending in the unused part of the array',
    input:  { left: ['10px', '20px', '30px'],
              offset: [ 0, 0.2, 0.8, 0.6 ] },
    output: [keyframe(offset(0),   { left: '10px' }),
             keyframe(offset(0.2), { left: '20px' }),
             keyframe(offset(0.8), { left: '30px' })],
  },

  // ----------- Property-indexed keyframes: easing handling -----------

  {
    desc:   'a property-indexed keyframe without any specified easing',
    input:  { left: ['10px', '20px', '30px'] },
    output: [keyframe(computedOffset(0),   { left: '10px' }, 'linear'),
             keyframe(computedOffset(0.5), { left: '20px' }, 'linear'),
             keyframe(computedOffset(1),   { left: '30px' }, 'linear')],
  },
  {
    desc:   'a property-indexed keyframe with a single easing',
    input:  { left: ['10px', '20px', '30px'], easing: 'ease-in' },
    output: [keyframe(computedOffset(0),   { left: '10px' }, 'ease-in'),
             keyframe(computedOffset(0.5), { left: '20px' }, 'ease-in'),
             keyframe(computedOffset(1),   { left: '30px' }, 'ease-in')],
  },
  {
    desc:   'a property-indexed keyframe with an array of easings',
    input:  { left: ['10px', '20px', '30px'],
              easing: ['ease-in', 'ease-out', 'ease-in-out'] },
    output: [keyframe(computedOffset(0),   { left: '10px' }, 'ease-in'),
             keyframe(computedOffset(0.5), { left: '20px' }, 'ease-out'),
             keyframe(computedOffset(1),   { left: '30px' }, 'ease-in-out')],
  },
  {
    desc:   'a property-indexed keyframe with an array of easings that is too'
            + ' short',
    input:  { left: ['10px', '20px', '30px'],
              easing: ['ease-in', 'ease-out'] },
    output: [keyframe(computedOffset(0),   { left: '10px' }, 'ease-in'),
             keyframe(computedOffset(0.5), { left: '20px' }, 'ease-out'),
             keyframe(computedOffset(1),   { left: '30px' }, 'ease-in')],
  },
  {
    desc:   'a property-indexed keyframe with a single-element array of'
            + ' easings',
    input:  { left: ['10px', '20px', '30px'], easing: ['ease-in'] },
    output: [keyframe(computedOffset(0),   { left: '10px' }, 'ease-in'),
             keyframe(computedOffset(0.5), { left: '20px' }, 'ease-in'),
             keyframe(computedOffset(1),   { left: '30px' }, 'ease-in')],
  },
  {
    desc:   'a property-indexed keyframe with an empty array of easings',
    input:  { left: ['10px', '20px', '30px'], easing: [] },
    output: [keyframe(computedOffset(0),   { left: '10px' }, 'linear'),
             keyframe(computedOffset(0.5), { left: '20px' }, 'linear'),
             keyframe(computedOffset(1),   { left: '30px' }, 'linear')],
  },
  {
    desc:   'a property-indexed keyframe with an array of easings that is too'
            + ' long',
    input:  { left: ['10px', '20px', '30px'],
              easing: ['steps(1)', 'steps(2)', 'steps(3)', 'steps(4)'] },
    output: [keyframe(computedOffset(0),   { left: '10px' }, 'steps(1)'),
             keyframe(computedOffset(0.5), { left: '20px' }, 'steps(2)'),
             keyframe(computedOffset(1),   { left: '30px' }, 'steps(3)')],
  },

  // ----------- Property-indexed keyframes: composite handling -----------

  {
    desc:   'a property-indexed keyframe with a single composite operation',
    input:  { left: ['10px', '20px', '30px'], composite: 'add' },
    output: [keyframe(computedOffset(0),   { left: '10px' }, 'linear', 'add'),
             keyframe(computedOffset(0.5), { left: '20px' }, 'linear', 'add'),
             keyframe(computedOffset(1),   { left: '30px' }, 'linear', 'add')],
  },
  {
    desc:   'a property-indexed keyframe with a composite array',
    input:  { left: ['10px', '20px', '30px'],
              composite: ['add', 'replace', 'accumulate'] },
    output: [keyframe(computedOffset(0),   { left: '10px' },
                      'linear', 'add'),
             keyframe(computedOffset(0.5), { left: '20px' },
                      'linear', 'replace'),
             keyframe(computedOffset(1),   { left: '30px' },
                      'linear', 'accumulate')],
  },
  {
    desc:   'a property-indexed keyframe with a composite array that is too'
            + ' short',
    input:  { left: ['10px', '20px', '30px', '40px', '50px'],
              composite: ['add', 'replace'] },
    output: [keyframe(computedOffset(0),    { left: '10px' },
                      'linear', 'add'),
             keyframe(computedOffset(0.25), { left: '20px' },
                      'linear', 'replace'),
             keyframe(computedOffset(0.5),  { left: '30px' },
                      'linear', 'add'),
             keyframe(computedOffset(0.75), { left: '40px' },
                      'linear', 'replace'),
             keyframe(computedOffset(1),    { left: '50px' },
                      'linear', 'add')],
  },
  {
    desc:   'a property-indexed keyframe with a composite array that is too'
            + ' long',
    input:  { left: ['10px', '20px'],
              composite: ['add', 'replace', 'accumulate'] },
    output: [keyframe(computedOffset(0), { left: '10px' },
                      'linear', 'add'),
             keyframe(computedOffset(1), { left: '20px' },
                      'linear', 'replace')],
  },
  {
    desc:   'a property-indexed keyframe with a single-element composite array',
    input:  { left: ['10px', '20px', '30px'],
              composite: ['add'] },
    output: [keyframe(computedOffset(0),   { left: '10px' }, 'linear', 'add'),
             keyframe(computedOffset(0.5), { left: '20px' }, 'linear', 'add'),
             keyframe(computedOffset(1),   { left: '30px' }, 'linear', 'add')],
  },

  // ----------- Keyframe sequence: property handling -----------

  {
    desc:   'a one property one keyframe sequence',
    input:  [{ offset: 1, left: '10px' }],
    output: [keyframe(offset(1), { left: '10px' })],
  },
  {
    desc:   'a one property two keyframe sequence',
    input:  [{ offset: 0, left: '10px' },
             { offset: 1, left: '20px' }],
    output: [keyframe(offset(0), { left: '10px' }),
             keyframe(offset(1), { left: '20px' })],
  },
  {
    desc:   'a two property two keyframe sequence',
    input:  [{ offset: 0, left: '10px', top: '30px' },
             { offset: 1, left: '20px', top: '40px' }],
    output: [keyframe(offset(0), { left: '10px', top: '30px' }),
             keyframe(offset(1), { left: '20px', top: '40px' })],
  },
  {
    desc:   'a one shorthand property two keyframe sequence',
    input:  [{ offset: 0, margin: '10px' },
             { offset: 1, margin: '20px 30px 40px 50px' }],
    output: [keyframe(offset(0), { margin: '10px' }),
             keyframe(offset(1), { margin: '20px 30px 40px 50px' })],
  },
  {
    desc:   'a two property (a shorthand and one of its component longhands)'
            + ' two keyframe sequence',
    input:  [{ offset: 0, margin: '10px', marginTop: '20px' },
             { offset: 1, marginTop: '70px', margin: '30px 40px 50px 60px' }],
    output: [keyframe(offset(0), { margin: '10px', marginTop: '20px' }),
             keyframe(offset(1), { marginTop: '70px',
                                   margin: '30px 40px 50px 60px' })],
  },
  {
    desc:   'a two property keyframe sequence where one property is missing'
            + ' from the first keyframe',
    input:  [{ offset: 0, left: '10px' },
             { offset: 1, left: '20px', top: '30px' }],
    output: [keyframe(offset(0), { left: '10px' }),
             keyframe(offset(1), { left: '20px', top: '30px' })],
  },
  {
    desc:   'a two property keyframe sequence where one property is missing'
            + ' from the last keyframe',
    input:  [{ offset: 0, left: '10px', top: '20px' },
             { offset: 1, left: '30px' }],
    output: [keyframe(offset(0), { left: '10px', top: '20px' }),
             keyframe(offset(1), { left: '30px' })],
  },
  {
    desc:   'a one property two keyframe sequence that needs to stringify'
            + ' its values',
    input:  [{ offset: 0, opacity: 0 },
             { offset: 1, opacity: 1 }],
    output: [keyframe(offset(0), { opacity: '0' }),
             keyframe(offset(1), { opacity: '1' })],
  },
  {
    desc:   'a keyframe sequence with a CSS variable reference',
    input:  [{ left: 'var(--dist)' },
             { left: 'calc(var(--dist) + 100px)' }],
    output: [keyframe(computedOffset(0), { left: 'var(--dist)' }),
             keyframe(computedOffset(1), { left: 'calc(var(--dist) + 100px)' })]
  },
  {
    desc:   'a keyframe sequence with a CSS variable reference in a shorthand'
            + ' property',
    input:  [{ margin: 'var(--dist)' },
             { margin: 'calc(var(--dist) + 100px)' }],
    output: [keyframe(computedOffset(0),
                      { margin: 'var(--dist)' }),
             keyframe(computedOffset(1),
                      { margin: 'calc(var(--dist) + 100px)' })],
  },
  {
    desc:   'a keyframe sequence with a CSS variable as its property',
    input:  [{ '--custom': 'a' },
             { '--custom': 'b' }],
    output: [keyframe(computedOffset(0), { '--custom': 'a' }),
             keyframe(computedOffset(1), { '--custom': 'b' })]
  },

  // ----------- Keyframe sequence: offset handling -----------

  {
    desc:   'a keyframe sequence with duplicate values for a given interior'
            + ' offset',
    input:  [{ offset: 0.0, left: '10px' },
             { offset: 0.5, left: '20px' },
             { offset: 0.5, left: '30px' },
             { offset: 0.5, left: '40px' },
             { offset: 1.0, left: '50px' }],
    output: [keyframe(offset(0),   { left: '10px' }),
             keyframe(offset(0.5), { left: '20px' }),
             keyframe(offset(0.5), { left: '30px' }),
             keyframe(offset(0.5), { left: '40px' }),
             keyframe(offset(1),   { left: '50px' })],
  },
  {
    desc:   'a keyframe sequence with duplicate values for offsets 0 and 1',
    input:  [{ offset: 0, left: '10px' },
             { offset: 0, left: '20px' },
             { offset: 0, left: '30px' },
             { offset: 1, left: '40px' },
             { offset: 1, left: '50px' },
             { offset: 1, left: '60px' }],
    output: [keyframe(offset(0), { left: '10px' }),
             keyframe(offset(0), { left: '20px' }),
             keyframe(offset(0), { left: '30px' }),
             keyframe(offset(1), { left: '40px' }),
             keyframe(offset(1), { left: '50px' }),
             keyframe(offset(1), { left: '60px' })],
  },
  {
    desc:   'a two property four keyframe sequence',
    input:  [{ offset: 0, left: '10px' },
             { offset: 0, top: '20px' },
             { offset: 1, top: '30px' },
             { offset: 1, left: '40px' }],
    output: [keyframe(offset(0), { left: '10px' }),
             keyframe(offset(0), { top:  '20px' }),
             keyframe(offset(1), { top:  '30px' }),
             keyframe(offset(1), { left: '40px' })],
  },
  {
    desc:   'a single keyframe sequence with omitted offset',
    input:  [{ left: '10px' }],
    output: [keyframe(computedOffset(1), { left: '10px' })],
  },
  {
    desc:   'a single keyframe sequence with null offset',
    input:  [{ offset: null, left: '10px' }],
    output: [keyframe(computedOffset(1), { left: '10px' })],
  },
  {
    desc:   'a single keyframe sequence with string offset',
    input:  [{ offset: '0.5', left: '10px' }],
    output: [keyframe(offset(0.5), { left: '10px' })],
  },
  {
    desc:   'a single keyframe sequence with a single calc() offset',
    input:  [{ offset: 'calc(0.5)', left: '10px' }],
    output: [keyframe(offset(0.5), { left: '10px' })],
  },
  {
    desc:   'a one property keyframe sequence with some omitted offsets',
    input:  [{ offset: 0.00, left: '10px' },
             { offset: 0.25, left: '20px' },
             { left: '30px' },
             { left: '40px' },
             { offset: 1.00, left: '50px' }],
    output: [keyframe(offset(0),            { left: '10px' }),
             keyframe(offset(0.25),         { left: '20px' }),
             keyframe(computedOffset(0.5),  { left: '30px' }),
             keyframe(computedOffset(0.75), { left: '40px' }),
             keyframe(offset(1),            { left: '50px' })],
  },
  {
    desc:   'a one property keyframe sequence with some null offsets',
    input:  [{ offset: 0.00, left: '10px' },
             { offset: 0.25, left: '20px' },
             { offset: null, left: '30px' },
             { offset: null, left: '40px' },
             { offset: 1.00, left: '50px' }],
    output: [keyframe(offset(0),            { left: '10px' }),
             keyframe(offset(0.25),         { left: '20px' }),
             keyframe(computedOffset(0.5),  { left: '30px' }),
             keyframe(computedOffset(0.75), { left: '40px' }),
             keyframe(offset(1),            { left: '50px' })],
  },
  {
    desc:   'a two property keyframe sequence with some omitted offsets',
    input:  [{ offset: 0.00, left: '10px', top: '20px' },
             { offset: 0.25, left: '30px' },
             { left: '40px' },
             { left: '50px', top: '60px' },
             { offset: 1.00, left: '70px', top: '80px' }],
    output: [keyframe(offset(0),            { left: '10px', top: '20px' }),
             keyframe(offset(0.25),         { left: '30px' }),
             keyframe(computedOffset(0.5),  { left: '40px' }),
             keyframe(computedOffset(0.75), { left: '50px', top: '60px' }),
             keyframe(offset(1),            { left: '70px', top: '80px' })],
  },
  {
    desc:   'a one property keyframe sequence with all omitted offsets',
    input:  [{ left: '10px' },
             { left: '20px' },
             { left: '30px' },
             { left: '40px' },
             { left: '50px' }],
    output: [keyframe(computedOffset(0),    { left: '10px' }),
             keyframe(computedOffset(0.25), { left: '20px' }),
             keyframe(computedOffset(0.5),  { left: '30px' }),
             keyframe(computedOffset(0.75), { left: '40px' }),
             keyframe(computedOffset(1),    { left: '50px' })],
  },

  // ----------- Keyframe sequence: easing handling -----------

  {
    desc:   'a keyframe sequence with different easing values, but the same'
            + ' easing value for a given offset',
    input:  [{ offset: 0.0, easing: 'ease',     left: '10px'},
             { offset: 0.0, easing: 'ease',     top: '20px'},
             { offset: 0.5, easing: 'linear',   left: '30px' },
             { offset: 0.5, easing: 'linear',   top: '40px' },
             { offset: 1.0, easing: 'step-end', left: '50px' },
             { offset: 1.0, easing: 'step-end', top: '60px' }],
    output: [keyframe(offset(0),   { left: '10px' }, 'ease'),
             keyframe(offset(0),   { top:  '20px' }, 'ease'),
             keyframe(offset(0.5), { left: '30px' }, 'linear'),
             keyframe(offset(0.5), { top:  '40px' }, 'linear'),
             keyframe(offset(1),   { left: '50px' }, 'steps(1)'),
             keyframe(offset(1),   { top:  '60px' }, 'steps(1)')],
  },

  // ----------- Keyframe sequence: composite handling -----------

  {
    desc:   'a keyframe sequence with different composite values, but the'
            + ' same composite value for a given offset',
    input:  [{ offset: 0.0, composite: 'replace', left: '10px' },
             { offset: 0.0, composite: 'replace', top: '20px' },
             { offset: 0.5, composite: 'add',     left: '30px' },
             { offset: 0.5, composite: 'add',     top: '40px' },
             { offset: 1.0, composite: 'replace', left: '50px' },
             { offset: 1.0, composite: 'replace', top: '60px' }],
    output: [keyframe(offset(0),   { left: '10px' }, 'linear', 'replace'),
             keyframe(offset(0),   { top:  '20px' }, 'linear', 'replace'),
             keyframe(offset(0.5), { left: '30px' }, 'linear', 'add'),
             keyframe(offset(0.5), { top:  '40px' }, 'linear', 'add'),
             keyframe(offset(1),   { left: '50px' }, 'linear', 'replace'),
             keyframe(offset(1),   { top:  '60px' }, 'linear', 'replace')],
  },
];

const gInvalidKeyframesTests = [
  {
    desc:  'keyframes with an out-of-bounded positive offset',
    input: [ { opacity: 0 },
             { opacity: 0.5, offset: 2 },
             { opacity: 1 } ],
  },
  {
    desc:  'keyframes with an out-of-bounded negative offset',
    input: [ { opacity: 0 },
             { opacity: 0.5, offset: -1 },
             { opacity: 1 } ],
  },
  {
    desc:  'property-indexed keyframes not loosely sorted by offset',
    input: { opacity: [ 0, 1 ], offset: [ 1, 0 ] },
  },
  {
    desc:  'property-indexed keyframes not loosely sorted by offset even'
           + ' though not all offsets are specified',
    input: { opacity: [ 0, 0.5, 1 ], offset: [ 0.5, 0 ] },
  },
  {
    desc:  'property-indexed keyframes with offsets out of range',
    input: { opacity: [ 0, 0.5, 1 ], offset: [ 0, 1.1 ] },
  },
  {
    desc:  'keyframes not loosely sorted by offset',
    input: [ { opacity: 0, offset: 1 },
             { opacity: 1, offset: 0 } ],
  },
  {
    desc:  'property-indexed keyframes with an invalid easing value',
    input: { opacity: [ 0, 0.5, 1 ],
             easing: 'inherit' },
  },
  {
    desc:  'property-indexed keyframes with an invalid easing value as one of'
           + ' the array values',
    input: { opacity: [ 0, 0.5, 1 ],
             easing: [ 'ease-in', 'inherit' ] },
  },
  {
    desc:   'property-indexed keyframe with an invalid easing in the unused'
            + ' part of the array of easings',
    input:  { left: ['10px', '20px', '30px'],
              easing: ['steps(1)', 'steps(2)', 'steps(3)', 'invalid'] },
  },
  {
    desc:   'empty property-indexed keyframe with an invalid easing',
    input:  { easing: 'invalid' },
  },
  {
    desc:   'empty property-indexed keyframe with an invalid easings array',
    input:  { easing: ['invalid'] },
  },
  {
    desc:  'a keyframe sequence with an invalid easing value',
    input: [ { opacity: 0, easing: 'jumpy' },
             { opacity: 1 } ],
  },
  {
    desc:  'property-indexed keyframes with an invalid composite value',
    input: { opacity: [ 0, 0.5, 1 ],
             composite: 'alternate' },
  },
  {
    desc:  'property-indexed keyframes with an invalid composite value as one'
           + ' of the array values',
    input: { opacity: [ 0, 0.5, 1 ],
             composite: [ 'add', 'alternate' ] },
  },
  {
    desc:  'keyframes with an invalid composite value',
    input: [ { opacity: 0, composite: 'alternate' },
             { opacity: 1 } ],
  },
];


const gKeyframeSerializationTests = [
  {
    desc:   'a on keyframe sequence which requires value serilaization of its'
            + ' values',
    input:  [{offset: 0, backgroundColor: 'rgb(1,2,3)' }],
    output: [keyframe(offset(0), { backgroundColor: 'rgb(1, 2, 3)' })],
  },
];



// ------------------------------
//  KeyframeEffectOptions
// ------------------------------

const gKeyframeEffectOptionTests = [
  {
    desc:     'an empty KeyframeEffectOptions object',
    input:    { },
    expected: { },
  },
  {
    desc:     'a normal KeyframeEffectOptions object',
    input:    { delay: 1000,
                fill: 'auto',
                iterations: 5.5,
                duration: 'auto',
                direction: 'alternate' },
    expected: { delay: 1000,
                fill: 'auto',
                iterations: 5.5,
                duration: 'auto',
                direction: 'alternate' },
  },
  {
    desc:     'a double value',
    input:    3000,
    expected: { duration: 3000 },
  },
  {
    desc:     '+Infinity',
    input:    Infinity,
    expected: { duration: Infinity },
  },
  {
    desc:     'an Infinity duration',
    input:    { duration: Infinity },
    expected: { duration: Infinity },
  },
  {
    desc:     'an auto duration',
    input:    { duration: 'auto' },
    expected: { duration: 'auto' },
  },
  {
    desc:     'an Infinity iterations',
    input:    { iterations: Infinity },
    expected: { iterations: Infinity },
  },
  {
    desc:     'an auto fill',
    input:    { fill: 'auto' },
    expected: { fill: 'auto' },
  },
  {
    desc:     'a forwards fill',
    input:    { fill: 'forwards' },
    expected: { fill: 'forwards' },
  }
];

const gInvalidKeyframeEffectOptionTests = [
  { desc: '-Infinity', input: -Infinity },
  { desc: 'NaN', input: NaN },
  { desc: 'a negative value', input: -1 },
  { desc: 'a negative Infinity duration', input: { duration: -Infinity } },
  { desc: 'a NaN duration', input: { duration: NaN } },
  { desc: 'a negative duration', input: { duration: -1 } },
  { desc: 'a string duration', input: { duration: 'merrychristmas' } },
  { desc: 'a negative Infinity iterations', input: { iterations: -Infinity} },
  { desc: 'a NaN iterations', input: { iterations: NaN } },
  { desc: 'a negative iterations', input: { iterations: -1 } },
  { desc: 'a blank easing', input: { easing: '' } },
  { desc: 'an unrecognized easing', input: { easing: 'unrecognised' } },
  { desc: 'an \'initial\' easing', input: { easing: 'initial' } },
  { desc: 'an \'inherit\' easing', input: { easing: 'inherit' } },
  { desc: 'a variable easing', input: { easing: 'var(--x)' } },
  { desc: 'a multi-value easing', input: { easing: 'ease-in-out, ease-out' } },
];

// There is currently only ScrollTimeline that can be constructed and used here
// beyond document timeline. Given that ScrollTimeline is not stable as of yet
// it's tested in scroll-animations/animation-with-animatable-interface.html.
const gAnimationTimelineTests = [
  {
    expectedTimeline: document.timeline,
    expectedTimelineDescription: 'document.timeline',
    description: 'with no timeline parameter'
  },
  {
    timeline: undefined,
    expectedTimeline: document.timeline,
    expectedTimelineDescription: 'document.timeline',
    description: 'with undefined timeline'
  },
  {
    timeline: null,
    expectedTimeline: null,
    expectedTimelineDescription: 'null',
    description: 'with null timeline'
  },
  {
    timeline: document.timeline,
    expectedTimeline: document.timeline,
    expectedTimelineDescription: 'document.timeline',
    description: 'with DocumentTimeline'
  },
];
