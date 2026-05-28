/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 * vi:set noexpandtab tabstop=8 shiftwidth=8:
 *
 * gs-nixos-source-dialog.c — dialog to choose NixOS install source
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <config.h>
#include <glib/gi18n.h>
#include <adwaita.h>

#include "gs-nixos-source-dialog.h"

struct _GsNixosSourceDialog
{
	AdwDialog		 parent;

	GsNixosInstallSource	 chosen_source;

	/* Template widgets */
	GtkLabel		*app_name_label;
	AdwPreferencesGroup	*sources_group;
	GtkButton		*install_button;

	/* Radio tracking */
	GtkCheckButton		*first_radio;
};

G_DEFINE_TYPE (GsNixosSourceDialog, gs_nixos_source_dialog, ADW_TYPE_DIALOG)

/* ---- helpers ---- */

typedef struct {
	GsNixosSourceDialog  *dialog;
	GsNixosInstallSource  source;
} RadioData;

static void
radio_toggled_cb (GtkCheckButton *radio, gpointer user_data)
{
	RadioData *data = user_data;
	if (gtk_check_button_get_active (radio)) {
		data->dialog->chosen_source = data->source;
		gtk_widget_set_sensitive (GTK_WIDGET (data->dialog->install_button), TRUE);
	}
}

static void
install_clicked_cb (GtkButton *button, GsNixosSourceDialog *self)
{
	adw_dialog_close (ADW_DIALOG (self));
}

static GtkCheckButton *
add_source_row (GsNixosSourceDialog  *self,
                const gchar          *title,
                const gchar          *subtitle,
                const gchar          *icon_name,
                GsNixosInstallSource  source)
{
	AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
	if (subtitle)
		adw_action_row_set_subtitle (row, subtitle);

	/* Icon */
	GtkImage *icon = GTK_IMAGE (gtk_image_new_from_icon_name (icon_name));
	adw_action_row_add_prefix (row, GTK_WIDGET (icon));

	/* Radio button */
	GtkCheckButton *radio = GTK_CHECK_BUTTON (gtk_check_button_new ());
	if (self->first_radio != NULL) {
		gtk_check_button_set_group (radio, self->first_radio);
	} else {
		self->first_radio = radio;
		self->chosen_source = source;
		gtk_check_button_set_active (radio, TRUE);
	}

	adw_action_row_add_suffix (row, GTK_WIDGET (radio));
	adw_action_row_set_activatable_widget (row, GTK_WIDGET (radio));

	RadioData *data = g_new0 (RadioData, 1);
	data->dialog = self;
	data->source = source;
	g_signal_connect_data (radio, "toggled",
	                       G_CALLBACK (radio_toggled_cb), data,
	                       (GClosureNotify) g_free, 0);

	adw_preferences_group_add (self->sources_group, GTK_WIDGET (row));
	return radio;
}

/* ---- GObject ---- */

static void
gs_nixos_source_dialog_class_init (GsNixosSourceDialogClass *klass)
{
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	gtk_widget_class_set_template_from_resource (widget_class,
	                                             "/org/gnome/Software/gs-nixos-source-dialog.ui");

	gtk_widget_class_bind_template_child (widget_class, GsNixosSourceDialog, app_name_label);
	gtk_widget_class_bind_template_child (widget_class, GsNixosSourceDialog, sources_group);
	gtk_widget_class_bind_template_child (widget_class, GsNixosSourceDialog, install_button);

	gtk_widget_class_bind_template_callback (widget_class, install_clicked_cb);
}

static void
gs_nixos_source_dialog_init (GsNixosSourceDialog *self)
{
	self->chosen_source = GS_NIXOS_INSTALL_SOURCE_NONE;
	self->first_radio = NULL;
	gtk_widget_init_template (GTK_WIDGET (self));
}

/* ---- Public API ---- */

GsNixosSourceDialog *
gs_nixos_source_dialog_new (GsApp    *app,
                             gboolean  has_system_backend,
                             gboolean  has_user_backend,
                             gboolean  has_profile_backend,
                             gboolean  has_flatpak)
{
	GsNixosSourceDialog *self = g_object_new (GS_TYPE_NIXOS_SOURCE_DIALOG, NULL);

	/* App name */
	const gchar *name = gs_app_get_name (app);
	if (name != NULL)
		gtk_label_set_text (self->app_name_label, name);

	/* Add available sources */
	if (has_system_backend) {
		add_source_row (self,
		                _("NixOS système"),
		                _("Ajoute dans configuration.nix et lance nixos-rebuild"),
		                "computer-symbolic",
		                GS_NIXOS_INSTALL_SOURCE_SYSTEM);
	}

	if (has_user_backend) {
		add_source_row (self,
		                _("Home Manager"),
		                _("Ajoute dans home.nix et lance home-manager switch"),
		                "user-home-symbolic",
		                GS_NIXOS_INSTALL_SOURCE_USER);
	}

	if (has_profile_backend) {
		g_autoptr(GSettings) settings = g_settings_new ("org.gnome.software");
		g_autofree gchar *user_backend = g_settings_get_string (settings, "nixos-user-backend");
		if (g_strcmp0 (user_backend, "env") == 0) {
			add_source_row (self,
			                _("Nix env (Legacy)"),
			                _("Installe avec nix-env -i (obsolète)"),
			                "package-x-generic-symbolic",
			                GS_NIXOS_INSTALL_SOURCE_ENV);
		} else {
			add_source_row (self,
			                _("Nix profile"),
			                _("Installe avec nix profile install (persistant)"),
			                "package-x-generic-symbolic",
			                GS_NIXOS_INSTALL_SOURCE_PROFILE);
		}
	}

	if (has_flatpak) {
		add_source_row (self,
		                _("Flatpak"),
		                _("Installe la version Flatpak (sandbox isolée)"),
		                "flatpak-symbolic",
		                GS_NIXOS_INSTALL_SOURCE_FLATPAK);
	}

	return self;
}

GsNixosInstallSource
gs_nixos_source_dialog_get_source (GsNixosSourceDialog *self)
{
	g_return_val_if_fail (GS_IS_NIXOS_SOURCE_DIALOG (self), GS_NIXOS_INSTALL_SOURCE_NONE);
	return self->chosen_source;
}
