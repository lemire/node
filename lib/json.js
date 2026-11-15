'use strict';

const {
  ArrayIsArray,
  ArrayPrototypeIncludes,
  ArrayPrototypeIndexOf,
  ArrayPrototypePush,
  ArrayPrototypeSymbolIterator,
  FunctionPrototypeCall,
  JSONStringify,
  MathTrunc,
  NumberIsFinite,
  ObjectDefineProperty,
  ObjectKeys,
  ObjectPrototypeHasOwnProperty,
  RegExpPrototypeExec,
  SafeMap,
  SafeSet,
  SafeWeakMap,
  StringPrototypeReplaceAll,
  SymbolIterator,
  SymbolToStringTag,
  Uint8Array,
} = primordials;

const { emitExperimentalWarning } = require('internal/util');
const { isAnyArrayBuffer, isArrayBufferView } = require('internal/util/types');
const {
  codes: {
    ERR_INVALID_ARG_TYPE,
    ERR_INVALID_ARG_VALUE,
  },
} = require('internal/errors');
const { inspect } = require('internal/util/inspect');

const binding = internalBinding('json');
const { JSONObject, JSONArray } = binding;

emitExperimentalWarning('node:json');

function toInput(input) {
  if (typeof input === 'string' || isArrayBufferView(input)) return input;
  if (isAnyArrayBuffer(input)) return new Uint8Array(input);
  throw new ERR_INVALID_ARG_TYPE(
    'input', ['string', 'Buffer', 'TypedArray', 'DataView', 'ArrayBuffer'],
    input);
}

// --------------------------------------------------------------------------
// Lazy views.

/**
 * Parses JSON into lazy, read-only views. Objects become JSONObject views,
 * arrays become JSONArray views; values are converted when accessed.
 * @param {string|ArrayBuffer|ArrayBufferView} input
 * @returns {JSONObject|JSONArray|string|number|boolean|null}
 */
function parse(input) {
  return binding.parse(toInput(input));
}

const objectProto = JSONObject.prototype;
const arrayProto = JSONArray.prototype;

const nativeKeys = objectProto.keys;
const nativeValues = objectProto.values;
const nativeEntries = objectProto.entries;
const nativeGetSize = objectProto.getSize;
const nativeArrayValues = arrayProto.values;
const nativeGetLength = arrayProto.getLength;
delete objectProto.getSize;
delete arrayProto.getLength;

function defineMethod(proto, name, value) {
  ObjectDefineProperty(proto, name, {
    __proto__: null, value, writable: true, configurable: true,
    enumerable: false,
  });
}

function defineGetter(proto, name, get) {
  ObjectDefineProperty(proto, name, {
    __proto__: null, get, configurable: true, enumerable: false,
  });
}

// Like Map: keys(), values() and entries() return iterators.
defineMethod(objectProto, 'keys', function keys() {
  return ArrayPrototypeSymbolIterator(FunctionPrototypeCall(nativeKeys, this));
});
defineMethod(objectProto, 'values', function values() {
  return ArrayPrototypeSymbolIterator(
    FunctionPrototypeCall(nativeValues, this));
});
defineMethod(objectProto, 'entries', function entries() {
  return ArrayPrototypeSymbolIterator(
    FunctionPrototypeCall(nativeEntries, this));
});
defineMethod(objectProto, SymbolIterator, objectProto.entries);
defineGetter(objectProto, 'size', function size() {
  return FunctionPrototypeCall(nativeGetSize, this);
});
defineMethod(objectProto, 'toObject', objectProto.toJSON);

defineMethod(arrayProto, 'values', function values() {
  return ArrayPrototypeSymbolIterator(
    FunctionPrototypeCall(nativeArrayValues, this));
});
defineMethod(arrayProto, SymbolIterator, arrayProto.values);
defineGetter(arrayProto, 'length', function length() {
  return FunctionPrototypeCall(nativeGetLength, this);
});
defineMethod(arrayProto, 'toArray', arrayProto.toJSON);

for (const { 0: proto, 1: name } of [[objectProto, 'JSONObject'],
                                     [arrayProto, 'JSONArray']]) {
  ObjectDefineProperty(proto, SymbolToStringTag, {
    __proto__: null, value: name, configurable: true,
  });
  defineMethod(proto, inspect.custom, function(depth, options) {
    if (depth < 0) return `[${name}]`;
    return `${name} ${inspect(this.toJSON(), {
      ...options,
      depth: options.depth == null ? null : options.depth - 1,
    })}`;
  });
}

// --------------------------------------------------------------------------
// Typed decoding with a subset of JSON Schema.

// Must match KindBit in src/node_json.cc.
const kNull = 1;
const kBoolean = 2;
const kInteger = 4;
const kNumber = 8;
const kString = 16;
const kObject = 32;
const kArray = 64;
const kAny = kNull | kBoolean | kInteger | kNumber | kString | kObject | kArray;

const kTypeBits = {
  __proto__: null,
  null: kNull,
  boolean: kBoolean,
  integer: kInteger,
  number: kInteger | kNumber,
  string: kString,
  object: kObject,
  array: kArray,
};

// Keywords that change what decode() returns or accepts. Annotations are
// ignored; any other validation keyword is rejected rather than silently
// ignored.
const kSupportedKeywords = new SafeSet([
  'type', 'properties', 'required', 'additionalProperties', 'items',
  'enum', 'const', 'default', 'nullable', '$ref', '$defs', 'definitions',
  '$schema', '$id', '$comment', 'title', 'description', 'examples',
  'deprecated', 'readOnly', 'writeOnly', 'format',
]);

const planCache = new SafeWeakMap();

function kindOf(value) {
  if (value === null) return kNull;
  switch (typeof value) {
    case 'boolean': return kBoolean;
    case 'string': return kString;
    case 'number':
      if (!NumberIsFinite(value)) return 0;
      return value === MathTrunc(value) ? kInteger | kNumber : kNumber;
    default: return 0;
  }
}

/**
 * Compiles a JSON Schema into the flat node list read by the native
 * compileSchema(). Node 0 is the root.
 * @param {object|boolean} root
 * @returns {object[]}
 */
function compileSchema(root) {
  const nodes = [];
  const memo = new SafeMap();
  let anyNode = -1;

  function fail(path, message) {
    throw new ERR_INVALID_ARG_VALUE(`schema${path}`, root, message);
  }

  function resolveRef(ref, path) {
    if (ref === '#') return root;
    const match = RegExpPrototypeExec(/^#\/(\$defs|definitions)\/(.+)$/, ref);
    if (match === null) {
      fail(path, `only local references such as "#/$defs/Name" are ` +
                 `supported, got ${JSONStringify(ref)}`);
    }
    const name = StringPrototypeReplaceAll(
      StringPrototypeReplaceAll(match[2], '~1', '/'), '~0', '~');
    const defs = root[match[1]];
    if (defs == null || !ObjectPrototypeHasOwnProperty(defs, name)) {
      fail(path, `unresolved reference ${JSONStringify(ref)}`);
    }
    return defs[name];
  }

  function visit(schema, path, seenRefs) {
    if (schema === true) {
      if (anyNode < 0) {
        anyNode = nodes.length;
        ArrayPrototypePush(nodes, { mask: kAny, items: -1 });
      }
      return anyNode;
    }
    if (typeof schema !== 'object' || schema === null || ArrayIsArray(schema)) {
      fail(path, 'must be an object or true');
    }
    if (schema.$ref !== undefined) {
      if (seenRefs.has(schema)) fail(path, 'reference cycle');
      seenRefs.add(schema);
      return visit(resolveRef(schema.$ref, path), path, seenRefs);
    }
    const known = memo.get(schema);
    if (known !== undefined) return known;

    const keywords = ObjectKeys(schema);
    for (let i = 0; i < keywords.length; i++) {
      if (!kSupportedKeywords.has(keywords[i])) {
        fail(path, `unsupported keyword ${JSONStringify(keywords[i])}`);
      }
    }

    const index = nodes.length;
    memo.set(schema, index);
    const node = { mask: 0, items: -1, additional: false };
    ArrayPrototypePush(nodes, node);

    let mask = 0;
    const { type } = schema;
    if (type === undefined) {
      if (schema.properties !== undefined) mask = kObject;
      else if (schema.items !== undefined) mask = kArray;
      else mask = kAny;
    } else {
      const types = ArrayIsArray(type) ? type : [type];
      for (let i = 0; i < types.length; i++) {
        const bits = kTypeBits[types[i]];
        if (bits === undefined) {
          fail(`${path}.type`, `unknown type ${JSONStringify(types[i])}`);
        }
        mask |= bits;
      }
    }
    if (schema.nullable === true) mask |= kNull;

    let values;
    if (schema.const !== undefined) values = [schema.const];
    if (schema.enum !== undefined) {
      if (!ArrayIsArray(schema.enum)) fail(`${path}.enum`, 'must be an array');
      values = schema.enum;
    }
    if (values !== undefined) {
      let enumMask = 0;
      for (let i = 0; i < values.length; i++) {
        const bits = kindOf(values[i]);
        if (bits === 0) {
          fail(`${path}.enum`, 'only strings, finite numbers, booleans ' +
                               'and null are supported');
        }
        enumMask |= bits;
      }
      mask = (type === undefined ? enumMask : mask & enumMask);
      node.enum = values;
    }
    node.mask = mask;

    if (schema.properties !== undefined) {
      const properties = schema.properties;
      if (typeof properties !== 'object' || properties === null) {
        fail(`${path}.properties`, 'must be an object');
      }
      const required = schema.required ?? [];
      if (!ArrayIsArray(required)) {
        fail(`${path}.required`, 'must be an array');
      }
      const keys = ObjectKeys(properties);
      node.keys = keys;
      node.props = [];
      node.required = [];
      node.defaultKinds = [];
      node.defaults = [];
      for (let i = 0; i < keys.length; i++) {
        const sub = properties[keys[i]];
        const subPath = `${path}.properties.${keys[i]}`;
        ArrayPrototypePush(node.props, visit(sub, subPath, new SafeSet()));
        ArrayPrototypePush(node.required,
                           ArrayPrototypeIncludes(required, keys[i]));
        const def = typeof sub === 'object' && sub !== null ?
          sub.default : undefined;
        if (def === undefined) {
          ArrayPrototypePush(node.defaultKinds, 0);
          ArrayPrototypePush(node.defaults, undefined);
        } else if (typeof def === 'object' && def !== null) {
          // Re-created for every decoded object, so instances never share it.
          ArrayPrototypePush(node.defaultKinds, 2);
          ArrayPrototypePush(node.defaults, JSONStringify(def));
        } else {
          ArrayPrototypePush(node.defaultKinds, 1);
          ArrayPrototypePush(node.defaults, def);
        }
      }
      for (let i = 0; i < required.length; i++) {
        if (ArrayPrototypeIndexOf(keys, required[i]) === -1) {
          fail(`${path}.required`,
               `${JSONStringify(required[i])} is not in properties`);
        }
      }
    }
    const additional = schema.additionalProperties;
    if (additional === true) {
      node.additional = true;
    } else if (additional !== undefined && additional !== false) {
      fail(`${path}.additionalProperties`, 'only true and false are supported');
    }

    if (schema.items !== undefined) {
      if (ArrayIsArray(schema.items)) {
        fail(`${path}.items`, 'tuple schemas are not supported');
      }
      node.items = visit(schema.items, `${path}.items`, new SafeSet());
    }
    return index;
  }

  visit(root, '', new SafeSet());
  return nodes;
}

function getPlan(schema) {
  let plan = planCache.get(schema);
  if (plan === undefined) {
    plan = binding.compileSchema(compileSchema(schema));
    planCache.set(schema, plan);
  }
  return plan;
}

/**
 * Parses JSON and builds plain objects and arrays shaped by a JSON Schema,
 * checking types as it goes. Properties not declared in the schema are
 * skipped unless `additionalProperties` is `true`.
 * @param {string|ArrayBuffer|ArrayBufferView} input
 * @param {object} schema
 * @returns {any}
 */
function decode(input, schema) {
  if (typeof schema !== 'object' || schema === null) {
    throw new ERR_INVALID_ARG_TYPE('schema', 'object', schema);
  }
  return binding.decode(toInput(input), getPlan(schema));
}

module.exports = {
  parse,
  decode,
  JSONObject,
  JSONArray,
};
