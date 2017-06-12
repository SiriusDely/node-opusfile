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
        '-pthread',
        '-fno-exceptions',
        '-fno-strict-aliasing',
        '-Wno-missing-field-initializers',
        '-pipe',
        '-fno-ident',
        '-fdata-sections',
        '-ffunction-sections',
        '-fPIC',
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
        'src/config/opusfile/<(OS)/<(target_arch)',
        'opusfile/include',
        'opusfile/src'
      ],
      'defines': [
        'LARGEFILE_SOURCE',
        '_FILE_OFFSET_BITS=64',
        'WEBRTC_TARGET_PC',
        'WEBRTC_LINUX',
        'WEBRTC_THREAD_RR',
        'EXPAT_RELATIVE_PATH',
        'GTEST_RELATIVE_PATH',
        'JSONCPP_RELATIVE_PATH',
        'WEBRTC_RELATIVE_PATH',
        'POSIX',
        '__STDC_FORMAT_MACROS',
        'DYNAMIC_ANNOTATIONS_ENABLED=0',
        'PIC',
        'HAVE_CONFIG_H'
      ],
      'link_settings': {
        'ldflags': [
        ],
        'libraries': [
          '-lopus',
          '-logg'
        ]
      }
    },
    {
      'target_name': 'node-opusfile',
      'dependencies': [
        'libopusfile'
      ],
      'include_dirs': [
        "<!(node -e \"require('nan')\")"
      ],
      'sources': [
        'src/node-opusfile.cc',
      ]
    }
  ]
}
