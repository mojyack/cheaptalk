project('cheaptalk', 'cpp', version: '1.0.0', default_options : ['warning_level=3', 'werror=false', 'cpp_std=c++23'])
add_project_arguments('-Wno-missing-field-initializers', language: 'cpp')
add_project_arguments('-Wfatal-errors', language : 'cpp')

subdir('src/net')
subdir('src/net/tcp')

pipewire_deps = [
  dependency('libspa-0.2'),
  dependency('libpipewire-0.3'),
]

opus_dep = dependency('opus')

executable('cheaptalk-server',
  files(
    'src/net.cpp',
    'src/opus.cpp',
    'src/server.cpp',
  )
  + netprotocol_files 
  + netprotocol_tcp_server_files, 
  dependencies : [opus_dep] + netprotocol_deps + netprotocol_tcp_deps,
  install : true,
)

executable('cheaptalk',
  files(
    'src/client.cpp',
    'src/net.cpp',
    'src/opus.cpp',
    'src/sound-pipewire.cpp',
  )
  + netprotocol_files 
  + netprotocol_tcp_client_files, 
  dependencies : pipewire_deps + [opus_dep] + netprotocol_deps + netprotocol_tcp_deps,
  install : true,
)

executable('pipewire-example',
  files(
    'examples/pipewire-capture-play-example.cpp',
  ),
  dependencies : pipewire_deps,
)

executable('opus-example',
  files(
    'examples/opus-example.cpp',
  ),
  dependencies : [opus_dep],
)
