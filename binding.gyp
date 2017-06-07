{
  "targets": [
    {
      'target_name': 'node-opusfile',
      'dependencies': [
        'opusfile.gyp:libopusfile'
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
