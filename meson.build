project('ncp','cpp',default_options:['cpp_std=c++17'])
message('compiler='+meson.get_compiler('cpp').get_id())
if(meson.get_compiler('cpp').get_id()=='clang++')
add_project_arguments(['-stdlib=libc++'], language : 'cpp')
add_project_link_arguments(['-stdlib=libc++','-lstdc++fs'], language : 'cpp')
endif
executable('ncp',sources:['src/main.cpp'])