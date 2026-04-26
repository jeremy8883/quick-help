project('quick-help', 'c',
  version : '0.1.0',
  default_options : ['warning_level=2', 'c_std=c11']
)

gtk4 = dependency('gtk4')
libcurl = dependency('libcurl')
json_glib = dependency('json-glib-1.0')

src = files(
  'src/main.c',
  'src/window.c',
  'src/ai.c',
  'src/window_context.c',
  'src/markdown.c',
)

executable('quick-help', src,
  dependencies : [gtk4, libcurl, json_glib],
  install : true
)
