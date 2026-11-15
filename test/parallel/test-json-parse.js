// Flags: --experimental-json --expose-gc
'use strict';
const common = require('../common');
const assert = require('assert');
const { parse, JSONObject, JSONArray } = require('node:json');

assert.throws(() => require('json'), { code: 'MODULE_NOT_FOUND' });

const text = JSON.stringify({
  a: 1,
  b: [1, 2, { c: 'x' }],
  d: { e: null, f: true, g: false },
  i64: -9007199254740991,
  u64: 18446744073709552000,  // Beyond uint64: simdjson's big integer path
  dbl: 1.5,
  s: 'héllo 😀\n"',
});
const expected = JSON.parse(text);

for (const input of [
  text,
  Buffer.from(text),
  new TextEncoder().encode(text),
  new TextEncoder().encode(text).buffer,
  new DataView(new TextEncoder().encode(text).buffer),
]) {
  const doc = parse(input);
  assert(doc instanceof JSONObject);
  assert.deepStrictEqual(doc.toJSON(), expected);
  assert.deepStrictEqual(doc.toObject(), expected);
  assert.strictEqual(JSON.stringify(doc), JSON.stringify(expected));
}

const doc = parse(text);
assert.strictEqual(doc.size, 7);
assert.strictEqual(doc.get('a'), 1);
assert.strictEqual(doc.get('missing'), undefined);
assert.strictEqual(doc.has('a'), true);
assert.strictEqual(doc.has('missing'), false);
assert.strictEqual(doc.get('i64'), expected.i64);
assert.strictEqual(doc.get('u64'), expected.u64);
assert.strictEqual(doc.get('dbl'), 1.5);
assert.strictEqual(doc.get('s'), expected.s);
assert.strictEqual(doc.get('d').get('e'), null);
assert.strictEqual(doc.get('d').get('f'), true);
assert.strictEqual(doc.get('d').get('g'), false);
assert.deepStrictEqual([...doc.keys()], Object.keys(expected));
assert.strictEqual([...doc.values()].length, 7);
assert.deepStrictEqual([...doc].map(([k]) => k), Object.keys(expected));
assert.strictEqual(Object.prototype.toString.call(doc), '[object JSONObject]');

const b = doc.get('b');
assert(b instanceof JSONArray);
assert.strictEqual(b.length, 3);
assert.strictEqual(b.at(0), 1);
assert.strictEqual(b.at(1), 2);
assert.strictEqual(b.at(-1).get('c'), 'x');
assert.strictEqual(b.at(0), 1);  // Moves the cursor backwards
assert.strictEqual(b.at(3), undefined);
assert.strictEqual(b.at(-4), undefined);
assert.strictEqual(b.at(1.7), 2);
assert.deepStrictEqual(b.toArray(), expected.b);
assert.strictEqual([...b].length, 3);

// JSON Pointer (RFC 6901).
const p = parse('{"a/b":{"m~n":[10,20]},"":5}');
assert.strictEqual(p.atPointer('/a~1b/m~0n/1'), 20);
assert.strictEqual(p.atPointer('/'), 5);
assert.strictEqual(p.atPointer('').size, 2);
assert.strictEqual(p.atPointer('/a~1b/m~0n/01'), undefined);
assert.strictEqual(p.atPointer('/a~1b/m~0n/2'), undefined);
assert.strictEqual(p.atPointer('/a~1b/m~0n/-'), undefined);
assert.strictEqual(p.atPointer('/x/y'), undefined);
assert.strictEqual(p.get('a/b').get('m~n').atPointer('/0'), 10);
assert.throws(() => p.atPointer('a'), { code: 'ERR_INVALID_ARG_VALUE' });
assert.throws(() => p.atPointer('/~2'), { code: 'ERR_INVALID_ARG_VALUE' });

// Scalar roots come back as plain values.
assert.strictEqual(parse('42'), 42);
assert.strictEqual(parse('"s"'), 's');
assert.strictEqual(parse('null'), null);
assert.deepStrictEqual(parse('[]').toArray(), []);
assert.deepStrictEqual(parse('{}').toObject(), {});

// Duplicate keys: get() returns the first, toObject() keeps the last.
const dup = parse('{"k":1,"k":2}');
assert.strictEqual(dup.get('k'), 1);
assert.deepStrictEqual(dup.toObject(), { k: 2 });
assert.deepStrictEqual(parse('{"__proto__":1}').toObject(),
                       JSON.parse('{"__proto__":1}'));

// More elements than the tape's 24-bit counter holds.
const many = parse(`[${'0,'.repeat(0x1000000)}1]`);
assert.strictEqual(many.length, 0x1000001);
assert.strictEqual(many.at(-1), 1);

// Errors.
assert.throws(() => parse('{bad'), SyntaxError);
assert.throws(() => parse(''), SyntaxError);
assert.throws(() => parse(1), { code: 'ERR_INVALID_ARG_TYPE' });
assert.throws(() => new JSONObject(), { code: 'ERR_ILLEGAL_CONSTRUCTOR' });
assert.throws(() => JSONObject.prototype.get.call({}, 'a'), TypeError);

// Views keep their document alive.
{
  let inner = parse('{"a":{"b":[1,{"c":"kept"}]}}').get('a').get('b');
  for (let i = 0; i < 5; i++) {
    global.gc();
    parse('{"other":"document"}');
  }
  assert.strictEqual(inner.at(1).get('c'), 'kept');
  inner = null;
}

// Each worker gets its own parser.
const { Worker } = require('worker_threads');
new Worker(`
  const { parse } = require('node:json');
  require('worker_threads').parentPort.postMessage(parse('{"x":[1,2,3]}').get('x').at(2));
`, { eval: true, execArgv: ['--experimental-json'] })
  .on('message', common.mustCall((v) => assert.strictEqual(v, 3)));
