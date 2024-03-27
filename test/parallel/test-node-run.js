'use strict';

const common = require('../common');
const { it, describe } = require('node:test');
const assert = require('node:assert');

const fixtures = require('../common/fixtures');

describe('node run [command]', () => {
  it('returns error on non-existent command', async () => {
    const child = await common.spawnPromisified(
      process.execPath,
      [ 'run', 'test'],
      { cwd: __dirname },
    );
    assert.match(child.stdout, /Can't read package\.json/);
    assert.strictEqual(child.stderr, '');
    assert.strictEqual(child.code, 1);
  });

  it('runs a valid command', async () => {
    // Run a script that just log `no test specified`
    const child = await common.spawnPromisified(
      process.execPath,
      [ 'run', 'test'],
      { cwd: fixtures.path('run-script') },
    );
    assert.match(child.stdout, /Error: no test specified/);
    assert.strictEqual(child.code, 1);
  });

  let adabatch = 'ada';
  if (common.isWindows) {
    adabatch = 'ada.bat';
  }

  it('adds node_modules/.bin to path', async () => {
    const child = await common.spawnPromisified(
      process.execPath,
      [ 'run', adabatch],
      { cwd: fixtures.path('run-script') },
    );
    assert.match(child.stdout, /06062023/);
    assert.strictEqual(child.stderr, '');
    assert.strictEqual(child.code, 0);
  });

  let posbatch = 'positional-args';
  if (common.isWindows) {
    posbatch = 'positional-args.bat';
  }
  it('appends positional arguments', async () => {
    const child = await common.spawnPromisified(
      process.execPath,
      [ 'run', posbatch, '--help "hello world test"'],
      { cwd: fixtures.path('run-script') },
    );
    assert.match(child.stdout, /--help ["]*hello world test.*/);
    assert.strictEqual(child.stderr, '');
    assert.strictEqual(child.code, 0);
  });

  let cusbatch = 'custom-env';
  if (common.isWindows) {
    cusbatch = 'custom-env.bat';
  }
  it('should support having --env-file cli flag', async () => {
    const child = await common.spawnPromisified(
      process.execPath,
      [ `--env-file=${fixtures.path('run-script/.env')}`, 'run', cusbatch],
      { cwd: fixtures.path('run-script') },
    );
    assert.match(child.stdout, /hello world/);
    assert.strictEqual(child.stderr, '');
    assert.strictEqual(child.code, 0);
  });
});
