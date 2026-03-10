// Test k-rate vs a-rate AudioParams.
//
// |options| describes how the testing of the AudioParam should be done:
//
//   sourceNodeName: name of source node to use for testing; defaults to
//                   'OscillatorNode'.  If set to 'none', then no source node
//                   is created for testing and it is assumed that the AudioNode
//                   under test are sources and need to be started.
//   verifyPieceWiseConstant: if true, verify that the k-rate output is
//                            piecewise constant for each render quantum.
//   nodeName:  name of the AudioNode to be tested
//   nodeOptions:  options to be used in the AudioNode constructor
//
//   prefix: Prefix for all output messages (to make them unique for
//           testharness)
//
//   rateSettings: A vector of dictionaries specifying how to set the automation
//                 rate(s):
//       name: Name of the AudioParam
//       value: The automation rate for the AudioParam given by |name|.
//
//   automations: A vector of dictionaries specifying how to automate each
//                AudioParam:
//       name: Name of the AudioParam
//
//       methods: A vector of dictionaries specifying the automation methods to
//                be used for testing:
//           name: Automation method to call
//           options: Arguments for the automation method
//
// Testing is somewhat rudimentary.  We create two nodes of the same type.  One
// node uses the default automation rates for each AudioParam (expecting them to
// be a-rate).  The second node sets the automation rate of AudioParams to
// "k-rate".  The set is speciified by |options.rateSettings|.
//
// For both of these nodes, the same set of automation methods (given by
// |options.automations|) is applied.  A simple oscillator is connected to each
// node which in turn are connected to different channels of an offline context.
// Channel 0 is the k-rate node output; channel 1, the a-rate output; and
// channel 2, the difference between the outputs.
//
// Success is declared if the difference signal is not exactly zero.  This means
// the automations did different things, as expected.
//
// The promise from |startRendering| is returned.
function doTest(context, should, options) {
  let merger = new ChannelMergerNode(
      context, {numberOfInputs: context.destination.channelCount});
  merger.connect(context.destination);

  let src = null;

  // Skip creating a source to drive the graph if |sourceNodeName| is 'none'.
  // If |sourceNodeName| is given, use that, else default to OscillatorNode.
  if (options.sourceNodeName !== 'none') {
    src = new window[options.sourceNodeName || 'OscillatorNode'](context);
  }

  let kRateNode = new window[options.nodeName](context, options.nodeOptions);
  let aRateNode = new window[options.nodeName](context, options.nodeOptions);
  let inverter = new GainNode(context, {gain: -1});

  // Set kRateNode filter to use k-rate params.
  options.rateSettings.forEach(setting => {
    kRateNode[setting.name].automationRate = setting.value;
    // Mostly for documentation in the output.  These should always
    // pass.
    should(
        kRateNode[setting.name].automationRate,
        `${options.prefix}: Setting ${
                                      setting.name
                                    }.automationRate to "${setting.value}"`)
        .beEqualTo(setting.value);
  });

  // Run through all automations for each node separately. (Mostly to keep
  // output of automations together.)
  options.automations.forEach(param => {
    param.methods.forEach(method => {
      // Most for documentation in the output.  These should never throw.
      let message = `${param.name}.${method.name}(${method.options})`
      should(() => {
        kRateNode[param.name][method.name](...method.options);
      }, options.prefix + ': k-rate node: ' + message).notThrow();
    });
  });
  options.automations.forEach(param => {
    param.methods.forEach(method => {
      // Most for documentation in the output.  These should never throw.
      let message = `${param.name}.${method.name}(${method.options})`
      should(() => {
        aRateNode[param.name][method.name](...method.options);
      }, options.prefix + ': a-rate node:' + message).notThrow();
    });
  });

  // Connect the source, if specified.
  if (src) {
    src.connect(kRateNode);
    src.connect(aRateNode);
  }

  // The k-rate result is channel 0, and the a-rate result is channel 1.
  kRateNode.connect(merger, 0, 0);
  aRateNode.connect(merger, 0, 1);

  // Compute the difference between the a-rate and k-rate results and send
  // that to channel 2.
  kRateNode.connect(merger, 0, 2);
  aRateNode.connect(inverter).connect(merger, 0, 2);

  if (src) {
    src.start();
  } else {
    // If there's no source, then assume the test nodes are sources and start
    // them.
    kRateNode.start();
    aRateNode.start();
  }

  return context.startRendering().then(renderedBuffer => {
    let kRateOutput = renderedBuffer.getChannelData(0);
    let aRateOutput = renderedBuffer.getChannelData(1);
    let diff = renderedBuffer.getChannelData(2);

    // Some informative messages to print out values of the k-rate and
    // a-rate outputs.  These should always pass.
    should(
        kRateOutput, `${options.prefix}: Output of k-rate ${options.nodeName}`)
        .beEqualToArray(kRateOutput);
    should(
        aRateOutput, `${options.prefix}: Output of a-rate ${options.nodeName}`)
        .beEqualToArray(aRateOutput);

    // The real test.  If k-rate AudioParam is working correctly, the
    // k-rate result MUST differ from the a-rate result.
    should(
        diff,
        `${
           options.prefix
         }: Difference between a-rate and k-rate ${options.nodeName}`)
        .notBeConstantValueOf(0);

    if (options.verifyPieceWiseConstant) {
      // Verify that the output from the k-rate parameter is step-wise
      // constant.
      for (let k = 0; k < kRateOutput.length; k += 128) {
        should(
            kRateOutput.slice(k, k + 128),
            `${options.prefix} k-rate output [${k}: ${k + 127}]`)
            .beConstantValueOf(kRateOutput[k]);
      }
    }
  });
}

// Test k-rate vs a-rate AudioParams.
//
// |options| describes how the testing of the AudioParam should be done:
//
//   sourceNodeName: name of source node to use for testing; defaults to
//                   'OscillatorNode'.  If set to 'none', then no source node
//                   is created for testing and it is assumed that the AudioNode
//                   under test are sources and need to be started.
//   verifyPieceWiseConstant: if true, verify that the k-rate output is
//                            piecewise constant for each render quantum.
//   nodeName:  name of the AudioNode to be tested
//   nodeOptions:  options to be used in the AudioNode constructor
//
//   prefix: Prefix for all output messages (to make them unique for
//           testharness)
//
//   rateSettings: A vector of dictionaries specifying how to set the automation
//                 rate(s):
//       name: Name of the AudioParam
//       value: The automation rate for the AudioParam given by |name|.
//
//   automations: A vector of dictionaries specifying how to automate each
//                AudioParam:
//       name: Name of the AudioParam
//
//       methods: A vector of dictionaries specifying the automation methods to
//                be used for testing:
//           name: Automation method to call
//           options: Arguments for the automation method
//
// Testing is somewhat rudimentary.  We create two nodes of the same type.  One
// node uses the default automation rates for each AudioParam (expecting them to
// be a-rate).  The second node sets the automation rate of AudioParams to
// "k-rate".  The set is speciified by |options.rateSettings|.
//
// For both of these nodes, the same set of automation methods (given by
// |options.automations|) is applied.  A simple oscillator is connected to each
// node which in turn are connected to different channels of an offline context.
// Channel 0 is the k-rate node output; channel 1, the a-rate output; and
// channel 2, the difference between the outputs.
//
// Success is declared if the difference signal is not exactly zero.  This means
// the automations did different things, as expected.
//
// The promise from |startRendering| is returned.
function doTest_W3TH(context, options) {
  const merger = new ChannelMergerNode(context, {
    numberOfInputs: context.destination.channelCount
  });
  merger.connect(context.destination);

  let src = null;

  // Skip creating a source to drive the graph if |sourceNodeName| is 'none'.
  // If |sourceNodeName| is given, use that, else default to OscillatorNode.
  if (options.sourceNodeName !== 'none') {
    src = new window[options.sourceNodeName || 'OscillatorNode'](context);
  }

  const kRateNode = new window[options.nodeName](context, options.nodeOptions);
  const aRateNode = new window[options.nodeName](context, options.nodeOptions);
  const inverter = new GainNode(context, { gain: -1 });

  // Set kRateNode filter to use k-rate params.
  options.rateSettings.forEach(setting => {
    kRateNode[setting.name].automationRate = setting.value;
    // Mostly for documentation in the output. These should always
    // pass.
    assert_equals(
        kRateNode[setting.name].automationRate, setting.value,
        `${options.prefix}: Setting ${setting.name}.automationRate ` +
            `to "${setting.value}"`);
  });

  // Run through all automations for each node separately. (Mostly to keep
  // output of automations together.)
  options.automations.forEach(param => {
    param.methods.forEach(method => {
      // Mostly for documentation in the output.  These should never throw.
      const message = `${param.name}.${method.name}(${method.options})`;
      try {
        kRateNode[param.name][method.name](...method.options);
      } catch (e) {
        assert_unreached(
            `${options.prefix}: k-rate node: ${message} threw: ${e.message}`);
      }
    });
  });
  options.automations.forEach(param => {
    param.methods.forEach(method => {
      // Mostly for documentation in the output.  These should never throw.
      const message = `${param.name}.${method.name}(${method.options})`;
      try {
        aRateNode[param.name][method.name](...method.options);
      } catch (e) {
        assert_unreached(
            `${options.prefix}: a-rate node: ${message} threw: ${e.message}`);
      }
    });
  });

  // Connect the source, if specified.
  if (src) {
    src.connect(kRateNode);
    src.connect(aRateNode);
  }

  // The k-rate result is channel 0, and the a-rate result is channel 1.
  kRateNode.connect(merger, 0, 0);
  aRateNode.connect(merger, 0, 1);

  // Compute the difference between the a-rate and k-rate results and send
  // that to channel 2.
  kRateNode.connect(merger, 0, 2);
  aRateNode.connect(inverter).connect(merger, 0, 2);

  if (src) {
    src.start();
  } else {
    // If there's no source, then assume the test nodes are sources and start
    // them.
    kRateNode.start();
    aRateNode.start();
  }

  return context.startRendering().then(renderedBuffer => {
    const kRateOutput = renderedBuffer.getChannelData(0);
    const aRateOutput = renderedBuffer.getChannelData(1);
    const diff = renderedBuffer.getChannelData(2);

    // Some informative messages to print out values of the k-rate and
    // a-rate outputs.  These should always pass.
    // (In testharness, assertions only report on failure,
    // so we sanity-check types.)
    assert_true(
        kRateOutput instanceof Float32Array,
        `${options.prefix}: Output of k-rate `+
            `${options.nodeName} is Float32Array`);
    assert_true(
        aRateOutput instanceof Float32Array,
        `${options.prefix}: Output of a-rate `+
            `${options.nodeName} is Float32Array`);

    // The real test.  If k-rate AudioParam is working correctly, the
    // k-rate result MUST differ from the a-rate result.
    let allZero = true;
    for (let i = 0; i < diff.length; ++i) {
      if (diff[i] !== 0) {allZero = false; break;}
    }
    assert_false(
        allZero,
        `${options.prefix}: Difference between a-rate and k-rate` +
            `${options.nodeName} must not be identically 0`);

    if (options.verifyPieceWiseConstant) {
      // Verify that the output from the k-rate parameter is step-wise
      // constant.
      for (let k = 0; k < kRateOutput.length; k += 128) {
        const end = Math.min(k + 128, kRateOutput.length);
        const v0 = kRateOutput[k];
        for (let i = k + 1; i < end; ++i) {
          assert_equals(
              kRateOutput[i],
              v0,
              `${options.prefix} k-rate output [${k}: ${end - 1}]` +
                  `should be piecewise constant`,
          );
        }
      }
    }
  });
}
