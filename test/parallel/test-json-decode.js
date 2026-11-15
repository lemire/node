// Flags: --experimental-json
'use strict';
require('../common');
const assert = require('assert');
const { decode } = require('node:json');

const User = {
  type: 'object',
  properties: {
    id: { type: 'integer' },
    login: { type: 'string' },
    followers: { type: 'integer', default: 0 },
  },
  required: ['id', 'login'],
};
const Repo = {
  type: 'object',
  properties: {
    id: { type: 'integer' },
    name: { type: 'string' },
    owner: User,
    topics: { type: 'array', items: { type: 'string' }, default: [] },
    visibility: { enum: ['public', 'private'], default: 'public' },
    license: { type: ['string', 'null'], default: null },
  },
  required: ['id', 'name', 'owner'],
};

const repo = '{"id":1,"name":"node","extra":{"x":[1]},' +
             '"owner":{"login":"nodejs","id":7,"site":"x"},"topics":["js"]}';
assert.deepStrictEqual(decode(repo, Repo), {
  id: 1,
  name: 'node',
  owner: { id: 7, login: 'nodejs', followers: 0 },
  topics: ['js'],
  visibility: 'public',
  license: null,
});
assert.deepStrictEqual(Object.keys(decode(repo, Repo).owner),
                       ['id', 'login', 'followers']);
assert.strictEqual(decode(`[${repo},${repo}]`,
                          { type: 'array', items: Repo }).length, 2);
assert.strictEqual(decode(Buffer.from(repo), Repo).name, 'node');

// Object and array defaults are not shared between results.
const minimal = '{"id":1,"name":"n","owner":{"id":7,"login":"x"}}';
const r1 = decode(minimal, Repo);
const r2 = decode(minimal, Repo);
assert.deepStrictEqual(r1.topics, []);
assert.notStrictEqual(r1.topics, r2.topics);

function invalid(input, message) {
  assert.throws(() => decode(input, Repo), {
    code: 'ERR_JSON_SCHEMA_VALIDATION',
    name: 'TypeError',
    message,
  });
}
invalid('{"id":1,"name":"n","owner":{"id":"7","login":"x"}}',
        'Expected integer, got string at $.owner.id');
invalid('{"id":1,"name":"n","owner":{"id":7}}',
        'Missing required property "login" at $.owner');
invalid('{"id":1.5,"name":"n","owner":{"id":7,"login":"x"}}',
        'Expected integer, got number at $.id');
invalid('{"id":1,"name":"n","owner":{"id":7,"login":"x"},"topics":["a",3]}',
        'Expected string, got integer at $.topics[1]');
invalid('{"id":1,"name":"n","owner":{"id":7,"login":"x"},"visibility":"x"}',
        'Value is not one of the allowed values at $.visibility');
invalid('[]', 'Expected object, got array at $');
assert.throws(() => decode('{', Repo), SyntaxError);

// Scalars.
assert.strictEqual(decode('1.0', { type: 'integer' }), 1);
assert.strictEqual(decode('1', { type: 'number' }), 1);
assert.strictEqual(decode('null', { type: 'string', nullable: true }), null);
assert.strictEqual(decode('3', { const: 3 }), 3);
assert.throws(() => decode('true', { const: 1 }),
              { code: 'ERR_JSON_SCHEMA_VALIDATION' });
assert.deepStrictEqual(decode('{"a":[1,{"b":2}]}', {}), { a: [1, { b: 2 }] });
assert.deepStrictEqual(decode('{"a":[1,{"b":2}]}', { type: 'object' }),
                       { a: [1, { b: 2 }] });

// Recursive schemas.
const Tree = {
  $defs: {
    node: {
      type: 'object',
      properties: {
        v: { type: 'integer' },
        kids: { type: 'array', items: { $ref: '#/$defs/node' } },
      },
    },
  },
  $ref: '#/$defs/node',
};
assert.deepStrictEqual(
  decode('{"v":1,"x":0,"kids":[{"v":2,"kids":[]},{"v":3}]}', Tree),
  { v: 1, kids: [{ v: 2, kids: [] }, { v: 3 }] });

// Keys that a DictionaryTemplate cannot hold, and additionalProperties.
assert.deepStrictEqual(
  decode('{"0":1,"a":2,"z":3}', {
    properties: { '0': true, 'a': true }, additionalProperties: true,
  }),
  { '0': 1, 'a': 2, 'z': 3 });

// Unsupported schemas are rejected up front.
for (const schema of [
  { type: 'integer', minimum: 3 },
  { anyOf: [{ type: 'string' }] },
  { type: 'nope' },
  { $ref: 'http://example.com/schema' },
  { items: [{ type: 'string' }] },
  { additionalProperties: { type: 'string' } },
  { properties: {}, required: ['a'] },
  { enum: [{}] },
]) {
  assert.throws(() => decode('1', schema), { code: 'ERR_INVALID_ARG_VALUE' });
}
assert.throws(() => decode('1', null), { code: 'ERR_INVALID_ARG_TYPE' });

// Big integers: an integer only while the double is finite.
assert.strictEqual(decode('1' + '0'.repeat(30), { type: 'integer' }), 1e30);
assert.throws(() => decode('1' + '0'.repeat(400), { type: 'integer' }),
              { code: 'ERR_JSON_SCHEMA_VALIDATION',
                message: 'Expected integer, got number at $' });
assert.strictEqual(decode('1' + '0'.repeat(400), { type: 'number' }),
                   Infinity);
assert(Object.is(decode('-0', { type: 'integer' }), -0));

// Duplicate keys: the last value wins and earlier ones are not checked.
const IntA = { properties: { a: { type: 'integer' } } };
assert.deepStrictEqual(decode('{"a":"bad","a":1}', IntA), { a: 1 });
assert.throws(() => decode('{"a":1,"a":"bad"}', IntA),
              { message: 'Expected integer, got string at $.a' });
assert.deepStrictEqual(
  decode('{"x":1,"x":2}', { properties: {}, additionalProperties: true }),
  { x: 2 });

// With nullable, enum and const accept null; const and enum intersect.
assert.strictEqual(decode('null', { enum: ['a'], nullable: true }), null);
assert.strictEqual(decode('"a"', { enum: ['a'], nullable: true }), 'a');
assert.strictEqual(decode('null', { const: 'a', nullable: true }), null);
assert.strictEqual(decode('1', { const: 1, enum: [1, 2] }), 1);
assert.throws(() => decode('2', { const: 1, enum: [1, 2] }),
              { code: 'ERR_JSON_SCHEMA_VALIDATION' });

// additionalProperties: false without properties keeps no property.
assert.deepStrictEqual(decode('{"a":1}', { additionalProperties: false }), {});
assert.strictEqual(decode('3', { additionalProperties: false }), 3);

// The schema `true` accepts anything, like {}.
assert.deepStrictEqual(decode('{"a":[1]}', true), { a: [1] });
assert.throws(() => decode('1', false), { code: 'ERR_INVALID_ARG_TYPE' });

// $ref: defaults on the target apply; one next to $ref overrides it.
{
  const defs = { $defs: { Vis: { enum: ['public', 'private'],
                                 default: 'public' } } };
  assert.deepStrictEqual(decode('{}', {
    ...defs, properties: { v: { $ref: '#/$defs/Vis' } },
  }), { v: 'public' });
  assert.deepStrictEqual(decode('{}', {
    ...defs, properties: { v: { $ref: '#/$defs/Vis', default: 'private' } },
  }), { v: 'private' });
}

// Defaults are checked and normalized when the schema is compiled.
assert.deepStrictEqual(
  decode('{}', { properties: { o: { properties: { a: { type: 'integer' } },
                                    default: { a: 1, extra: 2 } } } }),
  { o: { a: 1 } });
const circular = {};
circular.self = circular;
for (const def of [
  'one',                                  // Wrong type
  { toJSON() {} },                        // Not serializable
  circular,
  10n,
  NaN,
  { a: 'x' },                             // Nested mismatch
]) {
  assert.throws(() => decode('{}', {
    properties: { p: { properties: { a: { type: 'integer' } }, type: 'object',
                       default: def } },
  }), { code: 'ERR_INVALID_ARG_VALUE' });
}

// More schemas rejected up front.
for (const schema of [
  { $ref: '#/$defs/N', minimum: 1, $defs: { N: {} } },
  { $ref: '#/$defs/N', type: 'string', $defs: { N: {} } },
  { enum: [] },
  { type: 'string', enum: [1] },
  { const: 1, enum: [2] },
  { required: ['a'] },
]) {
  assert.throws(() => decode('1', schema), { code: 'ERR_INVALID_ARG_VALUE' });
}
