'use strict';

function assert_px_equals(observed, expected, description) {
  assert_equals(observed.unit, 'px',
                `Unexpected unit type for '${description}'`);
  assert_approx_equals(observed.value, expected, 0.0001,
                       `Unexpected value for ${description}`);
}

function CreateViewTimelineOpacityAnimation(test, target, options) {
  const timeline_options = {
    subject: target,
    axis: 'block'
  };
  if (options && 'timeline' in options) {
    for (let key in options.timeline) {
      timeline_options[key] = options.timeline[key];
    }
  }
  const animation_options = {
    timeline: new ViewTimeline(timeline_options)
  };
  if (options && 'animation' in options) {
    for (let key in options.animation) {
      animation_options[key] = options.animation[key];
    }
  }

  const anim =
      target.animate({ opacity: [0.3, 0.7] }, animation_options);
  test.add_cleanup(() => {
    anim.cancel();
  });
  return anim;
}

// Verify that range specified in the options aligns with the active range of
// the animation.
//
// Sample call:
// await runTimelineBoundsTest(t, {
//   timeline: { inset: [ CSS.percent(0), CSS.percent(20)] },
//   timing: { fill: 'both' }
//   startOffset: 600,
//   endOffset: 900
// });
async function runTimelineBoundsTest(t, options, message) {
  // Allow tiny sampling differences from subpixel scroll positions on
  // highdpi configurations.
  const opacityEpsilon = 0.001;
  const scrollOffsetProp = options.axis == 'block' ? 'scrollTop' : 'scrollLeft';
  container[scrollOffsetProp] = 0;
  await waitForNextFrame();

  const anim =
      options.anim ||
      CreateViewTimelineOpacityAnimation(t, target, options);
  if (options.timing)
    anim.effect.updateTiming(options.timing);

  const timeline = anim.timeline;
  await anim.ready;

  // Advance to the start offset, which triggers entry to the active phase.
  container[scrollOffsetProp] = options.startOffset;
  await waitForNextFrame();
  assert_approx_equals(Number(getComputedStyle(target).opacity), 0.3,
      opacityEpsilon,
      `Effect at the start of the active phase: ${message}`);

  // Advance to the midpoint of the animation.
  container[scrollOffsetProp] = (options.startOffset + options.endOffset) / 2;
  await waitForNextFrame();
  assert_approx_equals(Number(getComputedStyle(target).opacity), 0.5,
      opacityEpsilon,
      `Effect at the midpoint of the active range: ${message}`);

  // Advance to the end of the animation.
  container[scrollOffsetProp] = options.endOffset;
  await waitForNextFrame();
  assert_approx_equals(Number(getComputedStyle(target).opacity), 0.7,
      opacityEpsilon,
      `Effect is in the active phase at effect end time: ${message}`);

  // Return the animation so that we can continue testing with the same object.
  return anim;
}

// Sets the start and end range for a view timeline and ensures that the
// range aligns with expected values.
//
// Sample call:
// await runTimelineRangeTest(t, {
//   rangeStart: { rangeName: 'cover', offset: CSS.percent(0) } ,
//   rangeEnd: { rangeName: 'cover', offset: CSS.percent(100) },
//   startOffset: 600,
//   endOffset: 900
// });
async function runTimelineRangeTest(t, options) {
  const rangeToString = range => {
    const parts = [];
    if (range.rangeName)
      parts.push(range.rangeName);
    if (range.offset)
      parts.push(`${range.offset.value}%`);
    return parts.join(' ');
  };
  const range =
     `${rangeToString(options.rangeStart)} to ` +
     `${rangeToString(options.rangeEnd)}`;

  options.timeline = {
    axis: options.axis || 'inline'
  };
  options.animation = {
    rangeStart: options.rangeStart,
    rangeEnd: options.rangeEnd,
  };
  options.timing = {
    // Set fill to accommodate floating point precision errors at the
    // endpoints.
    fill: 'both'
  };

  return runTimelineBoundsTest(t, options, range);
}

// Sets the Inset for a view timeline and ensures that the range aligns with
// expected values.
//
// Sample call:
// await runTimelineInsetTest(t, {
//   inset: [ CSS.px(20), CSS.px(40) ]
//   startOffset: 600,
//   endOffset: 900
// });
async function runTimelineInsetTest(t, options) {
  options.timeline = {
    axis: 'inline',
    inset: options.inset
  };
  options.timing = {
    // Set fill to accommodate floating point precision errors at the
    // endpoints.
    fill: 'both'
  }
  const length = options.inset.length;
  const range =
      (options.inset instanceof Array) ? options.inset.join(' ')
                                       : options.inset;
  return runTimelineBoundsTest(t, options, range);
}
