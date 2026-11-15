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
