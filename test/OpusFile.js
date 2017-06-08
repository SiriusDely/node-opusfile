var OpusFile = require('..');

describe('OpusFile', function() {
  it('should convert ./test/data/input.opus to ./test/data/output.opus',
    function( done ) {
      OpusFile.Normalize('./test/data/input.opus', './test/data/output.opus');
      // number of converted frames: 392
      done();
  });
});
