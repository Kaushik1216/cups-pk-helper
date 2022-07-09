project ('cups-pk-helper',
         'c',
         version: '0.2.8',
         # Debian uses meson 0.49.2 in stable currently.
         meson_version : '>= 0.49.2',
         license: 'GPLv2+')

gnome = import ('gnome')
i18n = import ('i18n')

cc = meson.get_compiler ('c')

glib2_req = '>= 2.36.0'

glib2_dep = dependency ('glib-2.0', version : glib2_req)
gio2_dep = dependency ('gio-2.0', version : glib2_req)
gio_unix2_dep = dependency ('gio-unix-2.0', version : glib2_req)
gobject2_dep = dependency ('gobject-2.0', version : glib2_req)
polkit_dep = dependency ('polkit-gobject-1', version : '>= 0.97')
cups_dep = dependency ('cups', version : '>= 1.6')
pappl_dep = dependency ('pappl', version : '>=1.0')

subdir ('po')
subdir ('src')
