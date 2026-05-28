cargs = ['-DG_LOG_DOMAIN="GsPluginNixos"']

gs_plugin_nixos = shared_module(
  'gs_plugin_nixos',
  sources : 'gs-plugin-nixos.c',
  install : true,
  install_dir: plugin_dir,
  c_args : cargs,
  dependencies : plugin_libs,
)
