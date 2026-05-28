#pragma once

#include <gnome-software.h>

G_BEGIN_DECLS

#define GS_TYPE_PLUGIN_NIXOS (gs_plugin_nixos_get_type ())

G_DECLARE_FINAL_TYPE (GsPluginNixos, gs_plugin_nixos, GS, PLUGIN_NIXOS, GsPlugin)

G_END_DECLS
