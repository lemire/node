# JSON

<!--introduced_in=REPLACEME-->

<!-- YAML
added: REPLACEME
-->

> Stability: 1.0 - Early development. Enable this API with the
> [`--experimental-json`][] CLI flag.

<!-- source_link=lib/json.js -->

The `node:json` module parses JSON with [simdjson][] in two ways that
`JSON.parse()` does not offer:

* [`json.parse()`][] returns lazy, read-only views. The document is parsed at
  once, but JavaScript values are created only for the parts that are read.
* [`json.decode()`][] builds plain objects shaped by a [JSON Schema][],
  checking types as it goes and skipping properties the schema does not name.

Both are fastest when only part of a large document is needed. To convert a
whole document, `JSON.parse()` remains the better choice.

```mjs
import { parse, decode } from 'node:json';
```

```cjs
const { parse, decode } = require('node:json');
```

This module is only available under the `node:` scheme.

Both functions accept a string, a {Buffer}, a {TypedArray}, a {DataView} or an
{ArrayBuffer} holding UTF-8. Byte input avoids the UTF-8 conversion that a
string requires, so it suits data read from files or sockets. Invalid JSON
throws a {SyntaxError}.

## `json.parse(input)`

<!-- YAML
added: REPLACEME
-->

* `input` {string|Buffer|TypedArray|DataView|ArrayBuffer}
* Returns: {JSONObject|JSONArray|string|number|boolean|null}

Parses `input` and returns a view of its root. JSON objects become
[`JSONObject`][] views and arrays become [`JSONArray`][] views. Scalars are
returned as JavaScript values. The parsed document is copied into memory owned
by the views, so `input` can be changed or released as soon as `parse()`
returns. A view keeps its document alive.

```mjs
import { readFileSync } from 'node:fs';
import { parse } from 'node:json';

const doc = parse(readFileSync('twitter.json'));
const ids = [];
for (const status of doc.get('statuses')) {
  ids.push([status.get('id'), status.get('user').get('screen_name')]);
}
doc.atPointer('/statuses/0/user/name');  // JSON Pointer (RFC 6901)
doc.get('search_metadata').toObject();   // Converts a subtree, like JSON.parse
```

## Class: `JSONObject`

<!-- YAML
added: REPLACEME
-->

A read-only view of a JSON object, with a `Map`-like interface. Nested objects
and arrays are returned as views.

Lookups scan the object. When a key is repeated, `get()` and `has()` find the
first occurrence, while `toObject()` keeps the last, as `JSON.parse()` does.

### `jsonObject.get(key)`

* `key` {string}
* Returns: {any} The value, or `undefined` if `key` is absent.

### `jsonObject.has(key)`

* `key` {string}
* Returns: {boolean}

### `jsonObject.size`

* Type: {number} The number of members.

### `jsonObject.keys()`, `jsonObject.values()`, `jsonObject.entries()`

* Returns: {Iterator}

`jsonObject[Symbol.iterator]()` is the same as `jsonObject.entries()`.

### `jsonObject.atPointer(pointer)`

* `pointer` {string} A [JSON Pointer][], relative to this view.
* Returns: {any} The value, or `undefined` if there is none.

### `jsonObject.toObject()`

* Returns: {Object}

Converts the whole object, as `JSON.parse()` would. `toJSON()` is the same
method, so `JSON.stringify()` accepts views.

## Class: `JSONArray`

<!-- YAML
added: REPLACEME
-->

A read-only view of a JSON array.

### `jsonArray.at(index)`

* `index` {integer} Negative values count back from the end.
* Returns: {any} The element, or `undefined` if `index` is out of range.

Each view remembers the last position it reached, so reading the elements in
order is linear overall. Iterating is still the fastest way to visit them.

### `jsonArray.length`

* Type: {number}

### `jsonArray.values()`

* Returns: {Iterator}

`jsonArray[Symbol.iterator]()` is the same method.

### `jsonArray.atPointer(pointer)`

* `pointer` {string} A [JSON Pointer][], relative to this view.
* Returns: {any}

### `jsonArray.toArray()`

* Returns: {Array}

Converts the whole array, as `JSON.parse()` would. `toJSON()` is the same
method.

## `json.decode(input, schema)`

<!-- YAML
added: REPLACEME
-->

* `input` {string|Buffer|TypedArray|DataView|ArrayBuffer}
* `schema` {Object} A JSON Schema, in the subset described below.
* Returns: {any}

Parses `input` and builds the value described by `schema`, straight from the
parsed document. Every object described with `properties` gets exactly those
properties, in schema order, and is built without an intermediate object.
Properties of the input that the schema does not name are skipped, unless
`additionalProperties` is `true`. A value of the wrong type throws an
[`ERR_JSON_SCHEMA_VALIDATION`][] error that gives its location.

```mjs
import { decode } from 'node:json';

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

const repo = decode(data, Repo);
const repos = decode(data, { type: 'array', items: Repo });
// TypeError [ERR_JSON_SCHEMA_VALIDATION]:
//   Expected integer, got string at $.owner.id
```

A schema is compiled the first time it is used. The result is cached for as
long as the schema object is reachable, so reuse schema objects.

Supported keywords:

* `type`: a type name or an array of them: `'object'`, `'array'`, `'string'`,
  `'number'`, `'integer'` (`1.0` counts as an integer), `'boolean'` and
  `'null'`. `nullable: true` adds `'null'`.
* `properties`, `required`, and `additionalProperties` (`true` or `false`).
  A missing property takes its `default` if it has one. Object and array
  defaults are copied for each result.
* `items`, as a single schema.
* `enum` and `const`, with strings, finite numbers, booleans and `null`.
* `$ref` to `'#'`, `'#/$defs/Name'` or `'#/definitions/Name'`, which allows
  recursive schemas.
* `true` and `{}` accept any value, converted as `JSON.parse()` would.

Annotations such as `title`, `description` and `format` are ignored. Any other
keyword, such as `minimum`, `pattern` or `anyOf`, throws
`ERR_INVALID_ARG_VALUE` instead of being ignored.

Unlike in JSON Schema validation, an absent `additionalProperties` means
`false`: `decode()` drops the properties a schema does not name.

[JSON Pointer]: https://www.rfc-editor.org/rfc/rfc6901
[JSON Schema]: https://json-schema.org/
[`--experimental-json`]: cli.md#--experimental-json
[`ERR_JSON_SCHEMA_VALIDATION`]: errors.md#err_json_schema_validation
[`JSONArray`]: #class-jsonarray
[`JSONObject`]: #class-jsonobject
[`json.decode()`]: #jsondecodeinput-schema
[`json.parse()`]: #jsonparseinput
[simdjson]: https://simdjson.org/
