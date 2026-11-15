{
  'variables': {
    'simdjson_sources': [
      'simdjson.cpp',
    ]
  },
  'targets': [
     {
       'target_name': 'simdjson',
       'type': 'static_library',
       'include_dirs': ['.'],
       # Tape -0 as a double, so that it survives as -0 (as in JSON.parse).
       'defines': [ 'SIMDJSON_MINUS_ZERO_AS_FLOAT=1' ],
       'direct_dependent_settings': {
         'include_dirs': ['.'],
         'defines': [ 'SIMDJSON_MINUS_ZERO_AS_FLOAT=1' ],
       },
       'sources': [
         '<@(simdjson_sources)',
       ],
     },
  ]
}
