prefix = get_option ('prefix')
datadir = get_option ('datadir')
libexecdir = get_option ('libexecdir')

cups_pk_helper_mechanism_sources = files (
  'cups.c',
  'cups.h',
  'cups-pk-helper-mechanism.c',
  'cups-pk-helper-mechanism.h',
  'main.c'
)

cph_iface_mechanism_source = gnome.gdbus_codegen (
  'cph-iface-mechanism',
  'cups-pk-helper-mechanism.xml',
  interface_prefix : 'org.opensuse.CupsPkHelper.',
  namespace : 'CphIface',
  object_manager : true
)

executable (
  'cups-pk-helper-mechanism',
  cups_pk_helper_mechanism_sources,
  cph_iface_mechanism_source,
  install: true,
  install_dir: join_paths (prefix, libexecdir),
  dependencies : [glib2_dep, gio2_dep, gio_unix2_dep, polkit_dep, cups_dep, pappl_dep]
)


cph_sources = files (
  'cups.c',
  'cups.h',
)

tests = [
  'test-cups', 'test-cups-pk'
]

foreach test_name: tests
  bin = executable (
    test_name,
    test_name + '.c',
    cph_sources,
    cph_iface_mechanism_source,
    dependencies: [glib2_dep, gobject2_dep, gio2_dep, gio_unix2_dep, cups_dep, pappl_dep],
    c_args: ['-DG_DISABLE_DEPRECATED'])
  test (test_name, bin,
        env: ['G_TEST_SRCDIR=@0@'.format (meson.current_source_dir ()),
              'G_TEST_BUILDDIR=@0@'.format (meson.current_build_dir ())])
endforeach


# Install configuration file
install_data (
  'org.opensuse.CupsPkHelper.Mechanism.conf',
  install_dir: join_paths (prefix, datadir, 'dbus-1', 'system.d'),
)


# Substitute fields in service file and install it
dbus_conf = configuration_data ()
dbus_conf.set ('libexecdir', join_paths (prefix, libexecdir))
configure_file (
  input: 'org.opensuse.CupsPkHelper.Mechanism.service.in',
  output: 'org.opensuse.CupsPkHelper.Mechanism.service',
  configuration: dbus_conf,
)
install_data (
  meson.current_build_dir () / 'org.opensuse.CupsPkHelper.Mechanism.service',
  install_dir: join_paths (prefix, datadir, 'dbus-1', 'system-services')
)


# Translate and install policy file
i18n.merge_file (
  input: 'org.opensuse.cupspkhelper.mechanism.policy.in',
  output: 'org.opensuse.cupspkhelper.mechanism.policy',
  type: 'xml',
  data_dirs: join_paths (meson.source_root (), 'src'),
  po_dir: join_paths (meson.source_root (), 'po'),
  install: true,
  install_dir: join_paths (prefix, datadir, 'polkit-1', 'actions')
)
