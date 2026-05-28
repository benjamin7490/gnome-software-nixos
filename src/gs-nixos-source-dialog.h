/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 * vi:set noexpandtab tabstop=8 shiftwidth=8:
 *
 * gs-nixos-source-dialog.h — dialog to choose NixOS install source
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <adwaita.h>
#include <gnome-software.h>

G_BEGIN_DECLS

typedef enum {
	GS_NIXOS_INSTALL_SOURCE_NONE,
	GS_NIXOS_INSTALL_SOURCE_SYSTEM,    /* configuration.nix + nixos-rebuild */
	GS_NIXOS_INSTALL_SOURCE_USER,      /* home.nix + home-manager switch */
	GS_NIXOS_INSTALL_SOURCE_PROFILE,   /* nix profile install */
	GS_NIXOS_INSTALL_SOURCE_ENV,       /* nix-env -i */
	GS_NIXOS_INSTALL_SOURCE_FLATPAK,   /* handed off to flatpak plugin */
} GsNixosInstallSource;

#define GS_TYPE_NIXOS_SOURCE_DIALOG (gs_nixos_source_dialog_get_type ())
G_DECLARE_FINAL_TYPE (GsNixosSourceDialog, gs_nixos_source_dialog, GS, NIXOS_SOURCE_DIALOG, AdwDialog)

GsNixosSourceDialog   *gs_nixos_source_dialog_new           (GsApp                *app,
                                                              gboolean              has_system_backend,
                                                              gboolean              has_user_backend,
                                                              gboolean              has_profile_backend,
                                                              gboolean              has_flatpak);

GsNixosInstallSource   gs_nixos_source_dialog_get_source    (GsNixosSourceDialog  *self);

G_END_DECLS
