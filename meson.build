project('quic-echo', 'c',
  version: '0.1.0',
  license: 'MIT',
)

cmake = import('cmake')

add_project_arguments(['-D_GNU_SOURCE'],
                      language: 'c')

glib_dep = dependency('glib-2.0', fallback: ['glib', 'libglib_dep'])

libuv_dep = dependency('libuv', required : false)
if not libuv_dep.found()
  libuv_options = cmake.subproject_options()
  libuv_options.add_cmake_defines({
    'BUILD_TESTING': false,
  })
  libuv_project = cmake.subproject('libuv', options: libuv_options)
  libuv_dep = libuv_project.dependency('uv')
endif

ngtcp2_options = cmake.subproject_options()
ngtcp2_options.add_cmake_defines({
  'ENABLE_GNUTLS': false,
  'ENABLE_OPENSSL': false,
  'ENABLE_BORINGSSL': false,
  'BUILD_TESTING': false,
})
ngtcp2_project = cmake.subproject('ngtcp2', options: ngtcp2_options)
ngtcp2_dep = ngtcp2_project.dependency('ngtcp2')

libutils = static_library('libutils', [
                            'connection.c',
                            'plaintext.c',
                            'stream.c',
                            'utils.c',
                          ],
                          dependencies: [glib_dep , ngtcp2_dep , libuv_dep])

executable('cli', ['cli.c'],
           dependencies: [glib_dep , ngtcp2_dep , libuv_dep],
           link_with: libutils)

executable('serv', ['serv.c'],
           dependencies: [glib_dep , ngtcp2_dep, libuv_dep],
           link_with: libutils)

conf_data = configuration_data()
conf_data.set_quoted('G_LOG_DOMAIN', 'echo')
conf_data.set_quoted('GETTEXT_PACKAGE', meson.project_name())
configure_file(output : 'config.h',
               configuration : conf_data)
