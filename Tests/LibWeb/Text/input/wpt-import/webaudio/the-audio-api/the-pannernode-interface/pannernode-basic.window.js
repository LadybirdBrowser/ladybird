test((t) => {
  const context = new AudioContext();
  const source = new ConstantSourceNode(context);
  const panner = new PannerNode(context);
  source.connect(panner).connect(context.destination);

  // Basic parameters
  assert_equals(panner.numberOfInputs,1);
  assert_equals(panner.numberOfOutputs,1);
  assert_equals(panner.refDistance, 1);
  panner.refDistance = 270.5;
  assert_equals(panner.refDistance, 270.5);
  assert_equals(panner.maxDistance, 10000);
  panner.maxDistance = 100.5;
  assert_equals(panner.maxDistance, 100.5);
  assert_equals(panner.rolloffFactor, 1);
  panner.rolloffFactor = 0.75;
  assert_equals(panner.rolloffFactor, 0.75);
  assert_equals(panner.coneInnerAngle, 360);
  panner.coneInnerAngle = 240.5;
  assert_equals(panner.coneInnerAngle, 240.5);
  assert_equals(panner.coneOuterAngle, 360);
  panner.coneOuterAngle = 166.5;
  assert_equals(panner.coneOuterAngle, 166.5);
  assert_equals(panner.coneOuterGain, 0);
  panner.coneOuterGain = 0.25;
  assert_equals(panner.coneOuterGain, 0.25);
  assert_equals(panner.panningModel, 'equalpower');
  assert_equals(panner.distanceModel, 'inverse');

  // Position/orientation AudioParams
  assert_equals(panner.positionX.value, 0);
  assert_equals(panner.positionY.value, 0);
  assert_equals(panner.positionZ.value, 0);
  assert_equals(panner.orientationX.value, 1);
  assert_equals(panner.orientationY.value, 0);
  assert_equals(panner.orientationZ.value, 0);

  // AudioListener
  assert_equals(context.listener.positionX.value, 0);
  assert_equals(context.listener.positionY.value, 0);
  assert_equals(context.listener.positionZ.value, 0);
  assert_equals(context.listener.forwardX.value, 0);
  assert_equals(context.listener.forwardY.value, 0);
  assert_equals(context.listener.forwardZ.value, -1);
  assert_equals(context.listener.upX.value, 0);
  assert_equals(context.listener.upY.value, 1);
  assert_equals(context.listener.upZ.value, 0);

  panner.panningModel = 'equalpower';
  assert_equals(panner.panningModel, 'equalpower');
  panner.panningModel = 'HRTF';
  assert_equals(panner.panningModel, 'HRTF');
  panner.panningModel = 'invalid';
  assert_equals(panner.panningModel, 'HRTF');

  // Check that numerical values are no longer supported.  We shouldn't
  // throw and the value shouldn't be changed.
  panner.panningModel = 1;
  assert_equals(panner.panningModel, 'HRTF');

  panner.distanceModel = 'linear';
  assert_equals(panner.distanceModel, 'linear');
  panner.distanceModel = 'inverse';
  assert_equals(panner.distanceModel, 'inverse');
  panner.distanceModel = 'exponential';
  assert_equals(panner.distanceModel, 'exponential');

  panner.distanceModel = 'invalid';
  assert_equals(panner.distanceModel, 'exponential');
}, 'Test the PannerNode interface');
