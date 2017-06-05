# Build external deps.
{
  'variables': { 'target_arch%': 'x64' },

  'target_defaults': {
    'default_configuration': 'Debug',
    'configuration': {
      'Debug': {
        'defines': [ 'DEBUG', '_DEBUG' ],
        'msvs_settings': {
          'VSSLCompilerTool': {
            'RuntimeLibrary': 1, #static debug
          },
        },
      },
      'Release': {
        'defines': [ 'NODEBUG' ],
        'msvs_settings': {
          'VSSLCompilerTool': {
            'RuntimeLibrary': 0, #static release
          },
        },
      },
    },
    'msvs_settings': {
      'VCLinkerTool': {
        'GenerateDebugInformation': 'true',
      },
    },
  },

  'targets': [
    {
      'target_name': 'libopusfile',
      'type': 'static_library',
      'sources': [
        'opusfile/src/http.c',
        'opusfile/src/info.c',
        'opusfile/src/internal.c',
        'opusfile/src/opusfile.c',
        'opusfile/src/stream.c'
      ],
      'cflags': [
        '-fvisibility=hidden',
        '-W',
        '-Wstrict-prototypes',
        '-Wall',
        '-Wextra',
        '-Wcast-align',
        '-Wnested-externs',
        '-Wshadow',
        '-Wno-parentheses',
        '-Wno-unused-parameter',
        '-Wno-sign-compare',
        '-Wno-maybe-uninitialized'
      ],
      'include_dirs': [
        '/usr/local/include',
        '/usr/local/include/opus',
        'config/opusfile/<(OS)/<(target_arch)',
        'opusfile/include',
        'opusfile/src'
      ],
      'defines': [
        'PIC',
        'HAVE_CONFIG_H',
      ]
    }
  ]
}
