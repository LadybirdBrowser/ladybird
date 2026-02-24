let StereoPannerTest = (function() {

  // Constants
  let PI_OVER_TWO = Math.PI * 0.5;

  // Use a power of two to eliminate any round-off when converting frames to
  // time.
  let gSampleRate = 32768;

  // Time step when each panner node starts.  Make sure this is on a frame boundary.
  let gTimeStep = Math.floor(0.001 * gSampleRate) / gSampleRate;

  // How many panner nodes to create for the test
  let gNodesToCreate = 100;

  // Total render length for all of our nodes.
  let gRenderLength = gTimeStep * (gNodesToCreate + 1) + gSampleRate;

  // Calculates channel gains based on equal power panning model.
  // See: http://webaudio.github.io/web-audio-api/#panning-algorithm
  function getChannelGain(pan, numberOfChannels) {
    // The internal panning clips the pan value between -1, 1.
    pan = Math.min(Math.max(pan, -1), 1);
    let gainL, gainR;
    // Consider number of channels and pan value's polarity.
    if (numberOfChannels == 1) {
      let panRadian = (pan * 0.5 + 0.5) * PI_OVER_TWO;
      gainL = Math.cos(panRadian);
      gainR = Math.sin(panRadian);
    } else {
      let panRadian = (pan <= 0 ? pan + 1 : pan) * PI_OVER_TWO;
      if (pan <= 0) {
        gainL = 1 + Math.cos(panRadian);
        gainR = Math.sin(panRadian);
      } else {
        gainL = Math.cos(panRadian);
        gainR = 1 + Math.sin(panRadian);
      }
    }
    return {gainL: gainL, gainR: gainR};
  }


  /**
   * Test implementation class.
   * @param {Object} options Test options
   * @param {Object} options.description Test description
   * @param {Object} options.numberOfInputChannels Number of input channels
   */
  function Test(should, options) {
    // Primary test flag.
    this.success = true;

    this.should = should;
    this.context = null;
    this.prefix = options.prefix;
    this.numberOfInputChannels = (options.numberOfInputChannels || 1);
    switch (this.numberOfInputChannels) {
      case 1:
        this.description = 'Test for mono input';
        break;
      case 2:
        this.description = 'Test for stereo input';
        break;
    }

    // Onset time position of each impulse.
    this.onsets = [];

    // Pan position value of each impulse.
    this.panPositions = [];

    // Locations of where the impulses aren't at the expected locations.
    this.errors = [];

    // The index of the current impulse being verified.
    this.impulseIndex = 0;

    // The max error we allow between the rendered impulse and the
    // expected value.  This value is experimentally determined.  Set
    // to 0 to make the test fail to see what the actual error is.
    this.maxAllowedError = 1.284318e-7;

    // Max (absolute) error and the index of the maxima for the left
    // and right channels.
    this.maxErrorL = 0;
    this.maxErrorR = 0;
    this.maxErrorIndexL = 0;
    this.maxErrorIndexR = 0;

    // The maximum value to use for panner pan value. The value will range from
    // -panLimit to +panLimit.
    this.panLimit = 1.0625;
  }


  Test.prototype.init = function() {
    this.context = new OfflineAudioContext(2, gRenderLength, gSampleRate);
  };

  // Prepare an audio graph for testing. Create multiple impulse generators and
  // panner nodes, then play them sequentially while varying the pan position.
  Test.prototype.prepare = function() {
    let impulse;
    let impulseLength = Math.round(gTimeStep * gSampleRate);
    let sources = [];
    let panners = [];

    // Moves the pan value for each panner by pan step unit from -2 to 2.
    // This is to check if the internal panning value is clipped properly.
    let panStep = (2 * this.panLimit) / (gNodesToCreate - 1);

    if (this.numberOfInputChannels === 1) {
      impulse = createImpulseBuffer(this.context, impulseLength);
    } else {
      impulse = createStereoImpulseBuffer(this.context, impulseLength);
    }

    for (let i = 0; i < gNodesToCreate; i++) {
      sources[i] = this.context.createBufferSource();
      panners[i] = this.context.createStereoPanner();
      sources[i].connect(panners[i]);
      panners[i].connect(this.context.destination);
      sources[i].buffer = impulse;
      panners[i].pan.value = this.panPositions[i] = panStep * i - this.panLimit;

      // Store the onset time position of impulse.
      this.onsets[i] = gTimeStep * i;

      sources[i].start(this.onsets[i]);
    }
  };


  Test.prototype.verify = function() {
    let chanL = this.renderedBufferL;
    let chanR = this.renderedBufferR;
    for (let i = 0; i < chanL.length; i++) {
      // Left and right channels must start at the same instant.
      if (chanL[i] !== 0 || chanR[i] !== 0) {
        // Get amount of error between actual and expected gain.
        let expected = getChannelGain(
            this.panPositions[this.impulseIndex], this.numberOfInputChannels);
        let errorL = Math.abs(chanL[i] - expected.gainL);
        let errorR = Math.abs(chanR[i] - expected.gainR);

        if (errorL > this.maxErrorL) {
          this.maxErrorL = errorL;
          this.maxErrorIndexL = this.impulseIndex;
        }
        if (errorR > this.maxErrorR) {
          this.maxErrorR = errorR;
          this.maxErrorIndexR = this.impulseIndex;
        }

        // Keep track of the impulses that didn't show up where we expected
        // them to be.
        let expectedOffset =
            timeToSampleFrame(this.onsets[this.impulseIndex], gSampleRate);
        if (i != expectedOffset) {
          this.errors.push({actual: i, expected: expectedOffset});
        }

        this.impulseIndex++;
      }
    }
  };


  Test.prototype.showResult = function() {
    this.should(this.impulseIndex, this.prefix + 'Number of impulses found')
        .beEqualTo(gNodesToCreate);

    this.should(
            this.errors.length,
            this.prefix + 'Number of impulse at the wrong offset')
        .beEqualTo(0);

    this.should(this.maxErrorL, this.prefix + 'Left channel error magnitude')
        .beLessThanOrEqualTo(this.maxAllowedError);

    this.should(this.maxErrorR, this.prefix + 'Right channel error magnitude')
        .beLessThanOrEqualTo(this.maxAllowedError);
  };

  Test.prototype.run = function() {

    this.init();
    this.prepare();

    return this.context.startRendering().then(renderedBuffer => {
      this.renderedBufferL = renderedBuffer.getChannelData(0);
      this.renderedBufferR = renderedBuffer.getChannelData(1);
      this.verify();
      this.showResult();
    });
  };

  return {
    create: function(should, options) {
      return new Test(should, options);
    }
  };

})();
