/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 * vi:set noexpandtab tabstop=8 shiftwidth=8:
 *
 * Copyright (C) 2018 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include "gs-prefs-dialog.h"

#include "gnome-software-private.h"
#include "gs-common.h"
#include "gs-os-release.h"
#include "gs-repo-row.h"
#include <glib/gi18n.h>

struct _GsPrefsDialog
{
	AdwPreferencesDialog	 parent_instance;
	GSettings		*settings;

	GCancellable		*cancellable;
	GsPluginLoader		*plugin_loader;
	GtkWidget		*automatic_updates_radio;
	GtkWidget		*manual_updates_radio;
	GtkLabel                *updates_info_label;
	AdwActionRow		*automatic_updates_row;
	AdwActionRow		*manual_updates_row;
	AdwActionRow		*automatic_update_notifications_row;
	AdwActionRow		*show_only_free_apps_row;
	AdwActionRow		*show_only_verified_apps_row;

	GtkWidget		*nixos_page;
	GtkWidget		*nixos_mode_row;
	GtkWidget		*nixos_flake_uri_row;
	GtkWidget		*nixos_declarative_system_file_row;
	GtkWidget		*nixos_declarative_user_file_row;
	GtkWidget		*nixos_system_flake_dir_row;
	GtkWidget		*nixos_home_manager_flake_dir_row;
	GtkWidget		*nixos_option_steam_row;
	GtkWidget		*nixos_option_docker_row;
	GtkWidget		*nixos_option_virtualbox_row;
	GtkWidget		*nixos_option_wireshark_row;
	GtkWidget		*nixos_option_gparted_row;
	GtkWidget		*nixos_option_flatpak_row;
	GtkWidget		*nixos_declarative_group;
	GtkWidget		*nixos_options_group;
};

G_DEFINE_TYPE (GsPrefsDialog, gs_prefs_dialog, ADW_TYPE_PREFERENCES_DIALOG)

static void
gs_prefs_dialog_filters_changed_cb (GsPrefsDialog *self)
{
	g_signal_emit_by_name (self->plugin_loader, "reload", 0);
}

static void
popover_show_cb (GsPrefsDialog *self)
{
    const char *label = gtk_label_get_label (self->updates_info_label);

    gtk_accessible_announce (GTK_ACCESSIBLE (self),
                             label,
                             GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
}

static gboolean
gs_prefs_dialog_automatic_updates_to_radio_cb (GValue *value,
					       GVariant *variant,
					       gpointer user_data)
{
	GsPrefsDialog *self = user_data;
	if (!g_variant_get_boolean (variant))
		gtk_check_button_set_active (GTK_CHECK_BUTTON (self->manual_updates_radio), TRUE);
	g_value_set_boolean (value, g_variant_get_boolean (variant));
	return TRUE;
}

static void
gs_prefs_dialog_dispose (GObject *object)
{
	GsPrefsDialog *dialog = GS_PREFS_DIALOG (object);
	g_clear_object (&dialog->plugin_loader);
	g_cancellable_cancel (dialog->cancellable);
	g_clear_object (&dialog->cancellable);
	g_clear_object (&dialog->settings);

	G_OBJECT_CLASS (gs_prefs_dialog_parent_class)->dispose (object);
}

static gboolean
nixos_mode_to_selected (GValue *value, GVariant *variant, gpointer user_data)
{
	const gchar *mode = g_variant_get_string (variant, NULL);
	guint selected = 0;
	if (g_strcmp0 (mode, "nix-profile") == 0)
		selected = 0;
	else if (g_strcmp0 (mode, "nix-env") == 0)
		selected = 1;
	else if (g_strcmp0 (mode, "declarative-system") == 0)
		selected = 2;
	else if (g_strcmp0 (mode, "declarative-user") == 0)
		selected = 3;
	g_value_set_uint (value, selected);
	return TRUE;
}

static GVariant *
nixos_selected_to_mode (const GValue *value, const GVariantType *expected_type, gpointer user_data)
{
	guint selected = g_value_get_uint (value);
	const gchar *mode = "nix-profile";
	if (selected == 0)
		mode = "nix-profile";
	else if (selected == 1)
		mode = "nix-env";
	else if (selected == 2)
		mode = "declarative-system";
	else if (selected == 3)
		mode = "declarative-user";
	return g_variant_new_string (mode);
}

static void
nixos_mode_changed_cb (GsPrefsDialog *self)
{
	g_autofree gchar *mode = g_settings_get_string (self->settings, "nixos-mode");
	gboolean is_declarative = (g_strcmp0 (mode, "declarative-system") == 0 || g_strcmp0 (mode, "declarative-user") == 0);
	gboolean is_system = (g_strcmp0 (mode, "declarative-system") == 0);
	gboolean is_user = (g_strcmp0 (mode, "declarative-user") == 0);

	gtk_widget_set_sensitive (self->nixos_declarative_group, is_declarative);
	gtk_widget_set_sensitive (self->nixos_options_group, is_declarative);

	gtk_widget_set_sensitive (self->nixos_declarative_system_file_row, is_system);
	gtk_widget_set_sensitive (self->nixos_system_flake_dir_row, is_system);
	gtk_widget_set_sensitive (self->nixos_declarative_user_file_row, is_user);
	gtk_widget_set_sensitive (self->nixos_home_manager_flake_dir_row, is_user);
}

static void
gs_prefs_dialog_init (GsPrefsDialog *dialog)
{
	gtk_widget_init_template (GTK_WIDGET (dialog));

	dialog->cancellable = g_cancellable_new ();
	dialog->settings = g_settings_new ("org.gnome.software");
	g_settings_bind (dialog->settings,
			 "download-updates-notify",
			 dialog->automatic_update_notifications_row,
			 "active",
			 G_SETTINGS_BIND_DEFAULT);
	g_settings_bind_with_mapping (dialog->settings,
				      "download-updates",
				      dialog->automatic_updates_radio,
				      "active",
				      G_SETTINGS_BIND_DEFAULT,
				      gs_prefs_dialog_automatic_updates_to_radio_cb,
				      NULL, dialog, NULL);
	g_settings_bind (dialog->settings,
			 "show-only-free-apps",
			 dialog->show_only_free_apps_row,
			 "active",
			 G_SETTINGS_BIND_DEFAULT);
	g_settings_bind (dialog->settings,
			 "show-only-verified-apps",
			 dialog->show_only_verified_apps_row,
			 "active",
			 G_SETTINGS_BIND_DEFAULT);
	g_signal_connect_object (dialog->show_only_free_apps_row, "notify::active",
				 G_CALLBACK (gs_prefs_dialog_filters_changed_cb), dialog, G_CONNECT_SWAPPED);
	g_signal_connect_object (dialog->show_only_verified_apps_row, "notify::active",
				 G_CALLBACK (gs_prefs_dialog_filters_changed_cb), dialog, G_CONNECT_SWAPPED);

	gboolean on_nixos = g_file_test ("/run/current-system", G_FILE_TEST_EXISTS);
	if (!on_nixos) {
		g_autofree gchar *os_release_content = NULL;
		if (g_file_get_contents ("/etc/os-release", &os_release_content, NULL, NULL)) {
			if (g_strstr_len (os_release_content, -1, "ID=nixos") != NULL) {
				on_nixos = TRUE;
			}
		}
	}

	gtk_widget_set_visible (dialog->nixos_page, on_nixos);

	if (on_nixos) {
		g_settings_bind_with_mapping (dialog->settings,
					      "nixos-mode",
					      dialog->nixos_mode_row,
					      "selected",
					      G_SETTINGS_BIND_DEFAULT,
					      nixos_mode_to_selected,
					      nixos_selected_to_mode,
					      dialog, NULL);
		g_settings_bind (dialog->settings, "nixos-flake-uri", dialog->nixos_flake_uri_row, "text", G_SETTINGS_BIND_DEFAULT);
		g_settings_bind (dialog->settings, "nixos-declarative-system-file", dialog->nixos_declarative_system_file_row, "text", G_SETTINGS_BIND_DEFAULT);
		g_settings_bind (dialog->settings, "nixos-declarative-user-file", dialog->nixos_declarative_user_file_row, "text", G_SETTINGS_BIND_DEFAULT);
		g_settings_bind (dialog->settings, "nixos-system-flake-dir", dialog->nixos_system_flake_dir_row, "text", G_SETTINGS_BIND_DEFAULT);
		g_settings_bind (dialog->settings, "nixos-home-manager-flake-dir", dialog->nixos_home_manager_flake_dir_row, "text", G_SETTINGS_BIND_DEFAULT);
		g_settings_bind (dialog->settings, "nixos-option-steam", dialog->nixos_option_steam_row, "active", G_SETTINGS_BIND_DEFAULT);
		g_settings_bind (dialog->settings, "nixos-option-docker", dialog->nixos_option_docker_row, "active", G_SETTINGS_BIND_DEFAULT);
		g_settings_bind (dialog->settings, "nixos-option-virtualbox", dialog->nixos_option_virtualbox_row, "active", G_SETTINGS_BIND_DEFAULT);
		g_settings_bind (dialog->settings, "nixos-option-wireshark", dialog->nixos_option_wireshark_row, "active", G_SETTINGS_BIND_DEFAULT);
		g_settings_bind (dialog->settings, "nixos-option-gparted", dialog->nixos_option_gparted_row, "active", G_SETTINGS_BIND_DEFAULT);
		g_settings_bind (dialog->settings, "nixos-option-flatpak", dialog->nixos_option_flatpak_row, "active", G_SETTINGS_BIND_DEFAULT);

		g_signal_connect_object (dialog->settings,
					 "changed::nixos-mode",
					 G_CALLBACK (nixos_mode_changed_cb),
					 dialog, G_CONNECT_SWAPPED);
		nixos_mode_changed_cb (dialog);
	}
}

static void
gs_prefs_dialog_class_init (GsPrefsDialogClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	object_class->dispose = gs_prefs_dialog_dispose;

	gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Software/gs-prefs-dialog.ui");
	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, automatic_updates_radio);
	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, manual_updates_radio);
	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, updates_info_label);
	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, automatic_updates_row);
	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, manual_updates_row);
	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, automatic_update_notifications_row);
	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, show_only_free_apps_row);
	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, show_only_verified_apps_row);

	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, nixos_page);
	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, nixos_mode_row);
	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, nixos_flake_uri_row);
	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, nixos_declarative_system_file_row);
	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, nixos_declarative_user_file_row);
	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, nixos_system_flake_dir_row);
	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, nixos_home_manager_flake_dir_row);
	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, nixos_option_steam_row);
	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, nixos_option_docker_row);
	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, nixos_option_virtualbox_row);
	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, nixos_option_wireshark_row);
	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, nixos_option_gparted_row);
	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, nixos_option_flatpak_row);
	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, nixos_declarative_group);
	gtk_widget_class_bind_template_child (widget_class, GsPrefsDialog, nixos_options_group);

	gtk_widget_class_bind_template_callback (widget_class, popover_show_cb);
}

GsPrefsDialog *
gs_prefs_dialog_new (GsPluginLoader *plugin_loader)
{
	GsPrefsDialog *dialog;
	dialog = g_object_new (GS_TYPE_PREFS_DIALOG,
			       NULL);
	dialog->plugin_loader = g_object_ref (plugin_loader);
	return dialog;
}
