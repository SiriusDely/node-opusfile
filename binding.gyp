{
  "targets": [
    {
      'target_name': 'node-opusfile',
      'dependencies': [
        'deps/binding.gyp:libopusfile'
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
