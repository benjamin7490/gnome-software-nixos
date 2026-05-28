/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 * vi:set noexpandtab tabstop=8 shiftwidth=8:
 *
 * Copyright (C) 2026 GNOME Foundation, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <config.h>

#include <stdlib.h>
#include <string.h>
#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include <gnome-software.h>

#include "gs-plugin-nixos.h"

/* Install backends — can be combined simultaneously */
typedef enum {
	GS_NIXOS_BACKEND_NONE,
	GS_NIXOS_BACKEND_DECLARATIVE,   /* configuration.nix or home.nix */
	GS_NIXOS_BACKEND_PROFILE,       /* nix profile install */
	GS_NIXOS_BACKEND_ENV,           /* nix-env -i */
} GsNixosBackend;

/* Legacy single-mode enum kept for internal mapping only */
typedef enum {
	GS_PLUGIN_NIXOS_MODE_NIX_PROFILE,
	GS_PLUGIN_NIXOS_MODE_NIX_ENV,
	GS_PLUGIN_NIXOS_MODE_DECLARATIVE_SYSTEM,
	GS_PLUGIN_NIXOS_MODE_DECLARATIVE_USER
} GsPluginNixosMode;

struct _GsPluginNixos
{
	GsPlugin		parent;

	/* New dual-backend config */
	GsNixosBackend		system_backend;   /* declarative / none */
	GsNixosBackend		user_backend;     /* declarative / profile / env / none */
	gboolean		use_flakes;

	/* Legacy single-mode (derived from new config for compat) */
	GsPluginNixosMode	mode;

	gchar			*flake_uri;
	gchar			*declarative_system_file;
	gchar			*declarative_user_file;
	gchar			*system_flake_dir;
	gchar			*home_manager_flake_dir;

	GSettings		*settings;  /* kept alive to maintain changed signal */
	gboolean		in_plugin_action;
	guint			sync_timeout_id;
	GSubprocess		*sync_subprocess;
};

G_DEFINE_TYPE (GsPluginNixos, gs_plugin_nixos, GS_TYPE_PLUGIN)

typedef struct {
	GSubprocess *subprocess;
	GsPluginNixos *self;
} ListAppsData;

static void
list_apps_data_free (ListAppsData *data)
{
	g_clear_object (&data->subprocess);
	g_slice_free (ListAppsData, data);
}

static void
gs_plugin_nixos_init (GsPluginNixos *self)
{
	self->system_backend = GS_NIXOS_BACKEND_NONE;
	self->user_backend = GS_NIXOS_BACKEND_PROFILE;
	self->use_flakes = FALSE;
	self->mode = GS_PLUGIN_NIXOS_MODE_NIX_PROFILE;
	self->flake_uri = NULL;
	self->declarative_system_file = NULL;
	self->declarative_user_file = NULL;
	self->system_flake_dir = NULL;
	self->home_manager_flake_dir = NULL;
	self->settings = NULL;
	self->in_plugin_action = FALSE;
	self->sync_timeout_id = 0;
	self->sync_subprocess = NULL;
}

static void
gs_plugin_nixos_dispose (GObject *object)
{
	GsPluginNixos *self = GS_PLUGIN_NIXOS (object);

	if (self->sync_timeout_id != 0) {
		g_source_remove (self->sync_timeout_id);
		self->sync_timeout_id = 0;
	}
	g_clear_object (&self->sync_subprocess);
	g_clear_object (&self->settings);

	g_clear_pointer (&self->flake_uri, g_free);
	g_clear_pointer (&self->declarative_system_file, g_free);
	g_clear_pointer (&self->declarative_user_file, g_free);
	g_clear_pointer (&self->system_flake_dir, g_free);
	g_clear_pointer (&self->home_manager_flake_dir, g_free);

	G_OBJECT_CLASS (gs_plugin_nixos_parent_class)->dispose (object);
}

static gchar *
expand_path (const gchar *path)
{
	if (path == NULL)
		return NULL;
	if (g_str_has_prefix (path, "~/")) {
		return g_build_filename (g_get_home_dir (), path + 2, NULL);
	}
	return g_strdup (path);
}

static GsNixosBackend
parse_system_backend (const gchar *s)
{
	if (g_strcmp0 (s, "declarative") == 0) return GS_NIXOS_BACKEND_DECLARATIVE;
	return GS_NIXOS_BACKEND_NONE;
}

static GsNixosBackend
parse_user_backend (const gchar *s)
{
	if (g_strcmp0 (s, "declarative") == 0) return GS_NIXOS_BACKEND_DECLARATIVE;
	if (g_strcmp0 (s, "env") == 0)         return GS_NIXOS_BACKEND_ENV;
	if (g_strcmp0 (s, "none") == 0)        return GS_NIXOS_BACKEND_NONE;
	return GS_NIXOS_BACKEND_PROFILE; /* default */
}

static void
load_config (GsPluginNixos *self)
{
	g_autoptr(GSettings) settings = g_settings_new ("org.gnome.software");

	/* New dual-backend keys */
	g_autofree gchar *sys_str  = g_settings_get_string (settings, "nixos-system-backend");
	g_autofree gchar *user_str = g_settings_get_string (settings, "nixos-user-backend");
	self->system_backend = parse_system_backend (sys_str);
	self->user_backend   = parse_user_backend (user_str);
	self->use_flakes     = g_settings_get_boolean (settings, "nixos-use-flakes");

	/* Derive legacy mode for code that still uses it */
	if (self->system_backend == GS_NIXOS_BACKEND_DECLARATIVE)
		self->mode = GS_PLUGIN_NIXOS_MODE_DECLARATIVE_SYSTEM;
	else if (self->user_backend == GS_NIXOS_BACKEND_DECLARATIVE)
		self->mode = GS_PLUGIN_NIXOS_MODE_DECLARATIVE_USER;
	else if (self->user_backend == GS_NIXOS_BACKEND_ENV)
		self->mode = GS_PLUGIN_NIXOS_MODE_NIX_ENV;
	else
		self->mode = GS_PLUGIN_NIXOS_MODE_NIX_PROFILE;

	g_free (self->flake_uri);
	self->flake_uri = g_settings_get_string (settings, "nixos-flake-uri");

	g_free (self->declarative_system_file);
	self->declarative_system_file = expand_path (g_settings_get_string (settings, "nixos-declarative-system-file"));

	g_free (self->declarative_user_file);
	self->declarative_user_file = expand_path (g_settings_get_string (settings, "nixos-declarative-user-file"));

	g_free (self->system_flake_dir);
	self->system_flake_dir = expand_path (g_settings_get_string (settings, "nixos-system-flake-dir"));

	g_free (self->home_manager_flake_dir);
	self->home_manager_flake_dir = expand_path (g_settings_get_string (settings, "nixos-home-manager-flake-dir"));
}

typedef struct {
	const gchar *package_name;
	const gchar *option_nix;
} NixSpecialOption;

static const NixSpecialOption special_options[] = {
	{ "steam", "programs.steam.enable = true;" },
	{ "docker", "virtualisation.docker.enable = true;" },
	{ "virtualbox", "virtualisation.virtualbox.host.enable = true;" },
	{ "wireshark", "programs.wireshark.enable = true;" },
	{ "gparted", "programs.gparted.enable = true;" },
	{ "flatpak", "services.flatpak.enable = true;" },
};

static GPtrArray *
parse_nix_file_packages (const gchar *filepath)
{
	GPtrArray *packages = g_ptr_array_new_with_free_func (g_free);
	g_autofree gchar *content = NULL;
	gsize length = 0;
	g_autoptr(GError) error = NULL;

	if (!g_file_get_contents (filepath, &content, &length, &error)) {
		g_debug ("Could not read Nix file %s: %s", filepath, error->message);
		return packages;
	}

	for (guint i = 0; i < G_N_ELEMENTS (special_options); i++) {
		if (g_strstr_len (content, -1, special_options[i].option_nix) != NULL) {
			g_ptr_array_add (packages, g_strdup (special_options[i].package_name));
		}
	}

	const gchar *start = strchr (content, '[');
	if (start == NULL)
		return packages;
	start++;

	const gchar *end = strchr (start, ']');
	if (end == NULL)
		return packages;

	g_autofree gchar *in_between = g_strndup (start, end - start);
	g_auto(GStrv) tokens = g_strsplit_set (in_between, " \t\r\n", -1);
	for (guint i = 0; tokens[i] != NULL; i++) {
		g_strstrip (tokens[i]);
		if (tokens[i][0] == '\0' || tokens[i][0] == '#')
			continue;
		gboolean already_added = FALSE;
		for (guint j = 0; j < packages->len; j++) {
			if (g_strcmp0 (g_ptr_array_index (packages, j), tokens[i]) == 0) {
				already_added = TRUE;
				break;
			}
		}
		if (!already_added) {
			g_ptr_array_add (packages, g_strdup (tokens[i]));
		}
	}

	return packages;
}

static gboolean
write_nix_file_packages (const gchar *filepath, GPtrArray *packages, gboolean user_mode, GError **error)
{
	GString *content = g_string_new ("");
	g_string_append (content, "# This file is managed by GNOME Software. Manual changes may be overwritten.\n");
	g_string_append (content, "{ pkgs, ... }: {\n");
	if (user_mode) {
		g_string_append (content, "  home.packages = with pkgs; [\n");
	} else {
		g_string_append (content, "  environment.systemPackages = with pkgs; [\n");
	}

	for (guint i = 0; i < packages->len; i++) {
		const gchar *pkg = g_ptr_array_index (packages, i);
		gboolean is_special = FALSE;
		for (guint j = 0; j < G_N_ELEMENTS (special_options); j++) {
			if (g_strcmp0 (pkg, special_options[j].package_name) == 0) {
				is_special = TRUE;
				break;
			}
		}
		if (!is_special) {
			g_string_append_printf (content, "    %s\n", pkg);
		}
	}

	g_string_append (content, "  ];\n");

	g_autoptr(GSettings) settings = g_settings_new ("org.gnome.software");
	for (guint j = 0; j < G_N_ELEMENTS (special_options); j++) {
		g_autofree gchar *option_key = g_strdup_printf ("nixos-option-%s", special_options[j].package_name);
		if (g_settings_get_boolean (settings, option_key)) {
			g_string_append_printf (content, "  %s\n", special_options[j].option_nix);
		}
	}

	g_string_append (content, "}\n");

	gboolean res = g_file_set_contents (filepath, content->str, content->len, error);
	g_string_free (content, TRUE);
	return res;
}


static GsAppList *
list_declarative_packages (GsPlugin *plugin, const gchar *filepath, GError **error)
{
	g_autoptr(GsAppList) list = gs_app_list_new ();
	g_autoptr(GPtrArray) packages = parse_nix_file_packages (filepath);

	for (guint i = 0; i < packages->len; i++) {
		const gchar *name = g_ptr_array_index (packages, i);
		gboolean is_special = FALSE;
		for (guint j = 0; j < G_N_ELEMENTS (special_options); j++) {
			if (g_strcmp0 (name, special_options[j].package_name) == 0) {
				is_special = TRUE;
				break;
			}
		}
		if (is_special)
			continue;

		g_autoptr(GsApp) app = gs_app_new (name);
		gs_app_set_state (app, GS_APP_STATE_INSTALLED);
		gs_app_set_kind (app, AS_COMPONENT_KIND_GENERIC);
		gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_PACKAGE);
		gs_app_set_management_plugin (app, plugin);
		gs_app_add_source (app, name);
		gs_app_list_add (list, app);
	}

	g_autoptr(GSettings) settings = g_settings_new ("org.gnome.software");
	for (guint j = 0; j < G_N_ELEMENTS (special_options); j++) {
		g_autofree gchar *option_key = g_strdup_printf ("nixos-option-%s", special_options[j].package_name);
		if (g_settings_get_boolean (settings, option_key)) {
			g_autoptr(GsApp) app = gs_app_new (special_options[j].package_name);
			gs_app_set_state (app, GS_APP_STATE_INSTALLED);
			gs_app_set_kind (app, AS_COMPONENT_KIND_GENERIC);
			gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_PACKAGE);
			gs_app_set_management_plugin (app, plugin);
			gs_app_add_source (app, special_options[j].package_name);
			gs_app_list_add (list, app);
		}
	}

	return g_steal_pointer (&list);
}

static GsAppList *
parse_nix_profile_json (GsPlugin *plugin, const gchar *json_str, GError **error)
{
	g_autoptr(GsAppList) list = gs_app_list_new ();
	g_autoptr(JsonParser) parser = json_parser_new ();
	if (!json_parser_load_from_data (parser, json_str, -1, error)) {
		return NULL;
	}

	JsonNode *root = json_parser_get_root (parser);
	if (!JSON_NODE_HOLDS_OBJECT (root)) {
		g_set_error_literal (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_INVALID_FORMAT, "Expected JSON object");
		return NULL;
	}

	JsonObject *root_obj = json_node_get_object (root);
	if (!json_object_has_member (root_obj, "elements")) {
		return g_steal_pointer (&list);
	}

	JsonObject *elements = json_object_get_object_member (root_obj, "elements");
	GList *members = json_object_get_members (elements);
	for (GList *l = members; l != NULL; l = l->next) {
		const gchar *key = l->data;
		JsonObject *elem = json_object_get_object_member (elements, key);
		gboolean active = TRUE;
		if (json_object_has_member (elem, "active")) {
			active = json_object_get_boolean_member (elem, "active");
		}
		if (!active) continue;

		g_autoptr(GsApp) app = gs_app_new (key);
		gs_app_set_state (app, GS_APP_STATE_INSTALLED);
		gs_app_set_kind (app, AS_COMPONENT_KIND_GENERIC);
		gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_PACKAGE);
		gs_app_set_management_plugin (app, plugin);
		gs_app_add_source (app, key);
		gs_app_set_metadata (app, "NixOS::install-source", "profile");

		if (json_object_has_member (elem, "storePaths")) {
			JsonArray *paths = json_object_get_array_member (elem, "storePaths");
			if (json_array_get_length (paths) > 0) {
				const gchar *path = json_array_get_string_element (paths, 0);
				const gchar *base = g_strrstr (path, "/");
				if (base != NULL) {
					base++;
					if (strlen (base) > 33) {
						const gchar *name_ver = base + 33;
						const gchar *dash = strchr (name_ver, '-');
						while (dash != NULL && !g_ascii_isdigit (dash[1])) {
							dash = strchr (dash + 1, '-');
						}
						if (dash != NULL) {
							gs_app_set_version (app, dash + 1);
						}
					}
				}
			}
		}

		gs_app_list_add (list, app);
	}
	g_list_free (members);
	return g_steal_pointer (&list);
}

static GsAppList *
parse_nix_env_json (GsPlugin *plugin, const gchar *json_str, GError **error)
{
	g_autoptr(GsAppList) list = gs_app_list_new ();
	g_autoptr(JsonParser) parser = json_parser_new ();
	if (!json_parser_load_from_data (parser, json_str, -1, error)) {
		return NULL;
	}

	JsonNode *root = json_parser_get_root (parser);
	if (!JSON_NODE_HOLDS_OBJECT (root)) {
		g_set_error_literal (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_INVALID_FORMAT, "Expected JSON object");
		return NULL;
	}

	JsonObject *root_obj = json_node_get_object (root);
	GList *members = json_object_get_members (root_obj);
	for (GList *l = members; l != NULL; l = l->next) {
		const gchar *key = l->data;
		JsonObject *pkg = json_object_get_object_member (root_obj, key);
		const gchar *pname = key;
		const gchar *version = NULL;
		if (json_object_has_member (pkg, "pname")) {
			pname = json_object_get_string_member (pkg, "pname");
		}
		if (json_object_has_member (pkg, "version")) {
			version = json_object_get_string_member (pkg, "version");
		}

		g_autoptr(GsApp) app = gs_app_new (pname);
		gs_app_set_state (app, GS_APP_STATE_INSTALLED);
		gs_app_set_kind (app, AS_COMPONENT_KIND_GENERIC);
		gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_PACKAGE);
		gs_app_set_management_plugin (app, plugin);
		gs_app_add_source (app, pname);
		gs_app_set_metadata (app, "NixOS::install-source", "env");
		if (version != NULL) {
			gs_app_set_version (app, version);
		}

		gs_app_list_add (list, app);
	}
	g_list_free (members);
	return g_steal_pointer (&list);
}

/* Check if a binary exists under the NixOS system path — covers packages
 * installed via configuration.nix / environment.systemPackages which do
 * NOT show up in `nix profile list`. */
static gboolean
is_package_in_system_path (const gchar *package_name)
{
	/* Check /run/current-system/sw/bin/<name> */
	g_autofree gchar *bin_path = g_build_filename ("/run/current-system/sw/bin", package_name, NULL);
	if (g_file_test (bin_path, G_FILE_TEST_EXISTS))
		return TRUE;

	/* Also check /run/current-system/sw/lib/<name> for libraries */
	g_autofree gchar *lib_path = g_build_filename ("/run/current-system/sw/lib", package_name, NULL);
	if (g_file_test (lib_path, G_FILE_TEST_EXISTS))
		return TRUE;

	/* Check share/applications for .desktop files */
	g_autofree gchar *desktop_path = g_build_filename ("/run/current-system/sw/share/applications",
	                                                    package_name, NULL);
	g_autofree gchar *desktop_with_ext = g_strdup_printf ("%s.desktop", desktop_path);
	if (g_file_test (desktop_with_ext, G_FILE_TEST_EXISTS))
		return TRUE;

	return FALSE;
}

/* List all packages available via /run/current-system/sw/bin */
static GsAppList *
list_system_path_packages (GsPlugin *plugin, GError **error)
{
	g_autoptr(GsAppList) list = gs_app_list_new ();
	const gchar *sw_bin = "/run/current-system/sw/bin";
	g_autoptr(GDir) dir = g_dir_open (sw_bin, 0, error);
	if (dir == NULL)
		return NULL;

	const gchar *name;
	while ((name = g_dir_read_name (dir)) != NULL) {
		g_autoptr(GsApp) app = gs_app_new (name);
		gs_app_set_state (app, GS_APP_STATE_INSTALLED);
		gs_app_set_kind (app, AS_COMPONENT_KIND_GENERIC);
		gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_PACKAGE);
		gs_app_set_management_plugin (app, plugin);
		gs_app_add_source (app, name);
		gs_app_set_origin (app, "nixpkgs");
		gs_app_set_origin_ui (app, "NixOS (système)");
		gs_app_list_add (list, app);
	}
	return g_steal_pointer (&list);
}

static void
gs_plugin_nixos_adopt_app (GsPlugin *plugin, GsApp *app)
{
	AsBundleKind bundle_kind = gs_app_get_bundle_kind (app);
	if (bundle_kind == AS_BUNDLE_KIND_PACKAGE ||
	    bundle_kind == AS_BUNDLE_KIND_UNKNOWN) {
		/* Only adopt if it looks like a native package (not flatpak/snap) */
		const gchar *origin = gs_app_get_origin (app);
		if (origin == NULL || g_strcmp0 (origin, "flatpak") != 0)
			gs_app_set_management_plugin (app, plugin);
	}
}

static gboolean
nixos_sync_timeout_cb (gpointer user_data)
{
	GsPluginNixos *self = GS_PLUGIN_NIXOS (user_data);
	self->sync_timeout_id = 0;

	if (self->sync_subprocess != NULL) {
		if (!g_subprocess_get_if_exited (self->sync_subprocess) &&
		    !g_subprocess_get_if_signaled (self->sync_subprocess)) {
			/* Reschedule if still running */
			self->sync_timeout_id = g_timeout_add_seconds (2, nixos_sync_timeout_cb, self);
			return G_SOURCE_REMOVE;
		}
		g_clear_object (&self->sync_subprocess);
	}

	load_config (self);
	if (self->mode != GS_PLUGIN_NIXOS_MODE_DECLARATIVE_SYSTEM &&
	    self->mode != GS_PLUGIN_NIXOS_MODE_DECLARATIVE_USER) {
		return G_SOURCE_REMOVE;
	}

	gboolean user_mode = (self->mode == GS_PLUGIN_NIXOS_MODE_DECLARATIVE_USER);
	const gchar *filepath = user_mode ? self->declarative_user_file : self->declarative_system_file;

	g_autoptr(GPtrArray) packages = parse_nix_file_packages (filepath);
	g_autoptr(GError) error = NULL;

	if (user_mode) {
		if (!write_nix_file_packages (filepath, packages, TRUE, &error)) {
			g_warning ("Failed to write declarative user file: %s", error->message);
			return G_SOURCE_REMOVE;
		}
		g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func (g_free);
		g_ptr_array_add (argv, g_strdup ("home-manager"));
		g_ptr_array_add (argv, g_strdup ("switch"));
		g_ptr_array_add (argv, NULL);
		self->sync_subprocess = g_subprocess_newv ((const gchar * const *) argv->pdata,
		                                           G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
		                                           &error);
		if (self->sync_subprocess == NULL) {
			g_warning ("Failed to launch home-manager switch: %s", error->message);
		}
	} else {
		const gchar *tmp_file = "/tmp/gnome-software-packages.nix.tmp";
		if (!write_nix_file_packages (tmp_file, packages, FALSE, &error)) {
			g_warning ("Failed to write temporary system file: %s", error->message);
			return G_SOURCE_REMOVE;
		}
		g_autofree gchar *flake_nix = g_build_filename (self->system_flake_dir, "flake.nix", NULL);
		gboolean use_flake = g_file_test (flake_nix, G_FILE_TEST_EXISTS);

		g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func (g_free);
		g_ptr_array_add (argv, g_strdup ("pkexec"));
		g_ptr_array_add (argv, g_strdup ("sh"));
		g_ptr_array_add (argv, g_strdup ("-c"));
		if (use_flake) {
			g_ptr_array_add (argv, g_strdup_printf ("mv %s %s && nixos-rebuild switch --flake %s",
			                                        tmp_file, self->declarative_system_file, self->system_flake_dir));
		} else {
			g_ptr_array_add (argv, g_strdup_printf ("mv %s %s && nixos-rebuild switch",
			                                        tmp_file, self->declarative_system_file));
		}
		g_ptr_array_add (argv, NULL);

		self->sync_subprocess = g_subprocess_newv ((const gchar * const *) argv->pdata,
		                                           G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
		                                           &error);
		if (self->sync_subprocess == NULL) {
			g_warning ("Failed to launch nixos-rebuild switch: %s", error->message);
		}
	}

	return G_SOURCE_REMOVE;
}

static void
nixos_settings_changed_cb (GSettings *settings, const gchar *key, gpointer user_data)
{
	GsPluginNixos *self = GS_PLUGIN_NIXOS (user_data);
	if (self->in_plugin_action)
		return;

	if (g_str_has_prefix (key, "nixos-")) {
		if (g_strcmp0 (key, "nixos-mode") == 0 ||
		    g_str_has_prefix (key, "nixos-option-") ||
		    g_str_has_suffix (key, "-file") ||
		    g_str_has_suffix (key, "-dir")) {
			if (self->sync_timeout_id != 0) {
				g_source_remove (self->sync_timeout_id);
			}
			self->sync_timeout_id = g_timeout_add_seconds (2, nixos_sync_timeout_cb, self);
		}
	}
}

static void
gs_plugin_nixos_setup_async (GsPlugin *plugin,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
	GsPluginNixos *self = GS_PLUGIN_NIXOS (plugin);
	g_autoptr(GTask) task = g_task_new (plugin, cancellable, callback, user_data);
	g_task_set_source_tag (task, gs_plugin_nixos_setup_async);

	gboolean on_nixos = FALSE;
	if (g_file_test ("/run/current-system", G_FILE_TEST_EXISTS)) {
		on_nixos = TRUE;
	} else {
		g_autofree gchar *os_release_content = NULL;
		if (g_file_get_contents ("/etc/os-release", &os_release_content, NULL, NULL)) {
			if (g_strstr_len (os_release_content, -1, "ID=nixos") != NULL) {
				on_nixos = TRUE;
			}
		}
	}

	if (!on_nixos) {
		g_debug ("NixOS GS Plugin disabled: not running on NixOS");
		gs_plugin_set_enabled (plugin, FALSE);
		g_task_return_boolean (task, TRUE);
		return;
	}

	load_config (self);

	/* Keep settings alive so the changed signal stays connected */
	self->settings = g_settings_new ("org.gnome.software");
	g_signal_connect_object (self->settings, "changed",
	                         G_CALLBACK (nixos_settings_changed_cb), self, 0);

	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "appstream");
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_BEFORE, "packagekit");

	g_task_return_boolean (task, TRUE);
}

static gboolean
gs_plugin_nixos_setup_finish (GsPlugin *plugin, GAsyncResult *result, GError **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
}

static gchar *
guess_nix_package_name (GsApp *app)
{
	GPtrArray *sources = gs_app_get_sources (app);
	if (sources != NULL && sources->len > 0) {
		return g_strdup (g_ptr_array_index (sources, 0));
	}

	const gchar *app_id = gs_app_get_id (app);
	if (app_id == NULL)
		return NULL;

	const gchar *last_dot = g_strrstr (app_id, ".");
	gchar *name = NULL;
	if (last_dot != NULL) {
		name = g_ascii_strdown (last_dot + 1, -1);
	} else {
		name = g_ascii_strdown (app_id, -1);
	}

	return name;
}

static gboolean
is_package_in_nix_profile (JsonObject *elements, const gchar *package_name)
{
	GList *members = json_object_get_members (elements);
	gboolean found = FALSE;
	for (GList *l = members; l != NULL; l = l->next) {
		const gchar *key = l->data;
		JsonObject *elem = json_object_get_object_member (elements, key);
		if (json_object_has_member (elem, "storePaths")) {
			JsonArray *paths = json_object_get_array_member (elem, "storePaths");
			if (json_array_get_length (paths) > 0) {
				const gchar *path = json_array_get_string_element (paths, 0);
				const gchar *base = g_strrstr (path, "/");
				if (base != NULL) {
					base++;
					if (strlen (base) > 33) {
						g_autofree gchar *name_ver = g_strdup (base + 33);
						gchar *dash = strchr (name_ver, '-');
						while (dash != NULL && !g_ascii_isdigit (dash[1])) {
							dash = strchr (dash + 1, '-');
						}
						if (dash != NULL) {
							*dash = '\0';
						}
						if (g_strcmp0 (name_ver, package_name) == 0) {
							found = TRUE;
							break;
						}
					}
				}
			}
		}
	}
	g_list_free (members);
	return found;
}

static gboolean
is_package_in_nix_env (JsonObject *root_obj, const gchar *package_name)
{
	GList *members = json_object_get_members (root_obj);
	gboolean found = FALSE;
	for (GList *l = members; l != NULL; l = l->next) {
		const gchar *key = l->data;
		JsonObject *pkg = json_object_get_object_member (root_obj, key);
		const gchar *pname = key;
		if (json_object_has_member (pkg, "pname")) {
			pname = json_object_get_string_member (pkg, "pname");
		}
		if (g_strcmp0 (pname, package_name) == 0) {
			found = TRUE;
			break;
		}
	}
	g_list_free (members);
	return found;
}

static const gchar *
detect_package_install_source (GsPluginNixos *self, const gchar *package_name)
{
	/* Check System Declarative file */
	if (self->system_backend == GS_NIXOS_BACKEND_DECLARATIVE && self->declarative_system_file != NULL) {
		g_autoptr(GPtrArray) packages = parse_nix_file_packages (self->declarative_system_file);
		for (guint j = 0; j < packages->len; j++) {
			if (g_strcmp0 (g_ptr_array_index (packages, j), package_name) == 0)
				return "system";
		}
	}

	/* Check User Declarative file */
	if (self->user_backend == GS_NIXOS_BACKEND_DECLARATIVE && self->declarative_user_file != NULL) {
		g_autoptr(GPtrArray) packages = parse_nix_file_packages (self->declarative_user_file);
		for (guint j = 0; j < packages->len; j++) {
			if (g_strcmp0 (g_ptr_array_index (packages, j), package_name) == 0)
				return "user";
		}
	}

	/* Check Nix Profile */
	if (self->user_backend == GS_NIXOS_BACKEND_PROFILE) {
		g_autoptr(GSubprocess) subprocess = NULL;
		g_autoptr(GBytes) stdout_buf = NULL;
		subprocess = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
		                               "nix", "profile", "list", "--json", NULL);
		if (subprocess != NULL && g_subprocess_communicate (subprocess, NULL, NULL, &stdout_buf, NULL, NULL)) {
			const gchar *output = g_bytes_get_data (stdout_buf, NULL);
			if (output != NULL) {
				g_autoptr(JsonParser) parser = json_parser_new ();
				if (json_parser_load_from_data (parser, output, -1, NULL)) {
					JsonNode *root = json_parser_get_root (parser);
					if (JSON_NODE_HOLDS_OBJECT (root)) {
						JsonObject *root_obj = json_node_get_object (root);
						if (json_object_has_member (root_obj, "elements")) {
							JsonObject *elements = json_object_get_object_member (root_obj, "elements");
							if (is_package_in_nix_profile (elements, package_name))
								return "profile";
						}
					}
				}
			}
		}
	}

	/* Check Nix Env */
	if (self->user_backend == GS_NIXOS_BACKEND_ENV) {
		g_autoptr(GSubprocess) subprocess = NULL;
		g_autoptr(GBytes) stdout_buf = NULL;
		subprocess = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
		                               "nix-env", "-q", "--json", NULL);
		if (subprocess != NULL && g_subprocess_communicate (subprocess, NULL, NULL, &stdout_buf, NULL, NULL)) {
			const gchar *output = g_bytes_get_data (stdout_buf, NULL);
			if (output != NULL) {
				g_autoptr(JsonParser) parser = json_parser_new ();
				if (json_parser_load_from_data (parser, output, -1, NULL)) {
					JsonNode *root = json_parser_get_root (parser);
					if (JSON_NODE_HOLDS_OBJECT (root)) {
						JsonObject *root_obj = json_node_get_object (root);
						if (is_package_in_nix_env (root_obj, package_name))
							return "env";
					}
				}
			}
		}
	}

	/* Special options fallback check */
	if (self->system_backend == GS_NIXOS_BACKEND_DECLARATIVE) {
		g_autoptr(GSettings) settings = g_settings_new ("org.gnome.software");
		for (guint j = 0; j < G_N_ELEMENTS (special_options); j++) {
			if (g_strcmp0 (package_name, special_options[j].package_name) == 0) {
				g_autofree gchar *option_key = g_strdup_printf ("nixos-option-%s", package_name);
				if (g_settings_get_boolean (settings, option_key))
					return "system";
			}
		}
	}

	/* Fallback check on system path if it's there */
	if (is_package_in_system_path (package_name)) {
		return "system";
	}

	return NULL;
}

static gboolean
is_package_installed (GsPluginNixos *self, const gchar *package_name)
{
	return detect_package_install_source (self, package_name) != NULL;
}

static gchar *
get_nix_profile_index_by_package_name (GsPluginNixos *self, const gchar *package_name)
{
	gchar *index_str = NULL;
	g_autoptr(GSubprocess) subprocess = NULL;
	g_autoptr(GBytes) stdout_buf = NULL;
	g_autoptr(GError) local_error = NULL;
	subprocess = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
	                               &local_error,
	                               "nix", "profile", "list", "--json", NULL);
	if (subprocess != NULL && g_subprocess_communicate (subprocess, NULL, NULL, &stdout_buf, NULL, NULL)) {
		const gchar *output = g_bytes_get_data (stdout_buf, NULL);
		if (output != NULL) {
			g_autoptr(JsonParser) parser = json_parser_new ();
			if (json_parser_load_from_data (parser, output, -1, NULL)) {
				JsonNode *root = json_parser_get_root (parser);
				if (JSON_NODE_HOLDS_OBJECT (root)) {
					JsonObject *root_obj = json_node_get_object (root);
					if (json_object_has_member (root_obj, "elements")) {
						JsonObject *elements = json_object_get_object_member (root_obj, "elements");
						GList *members = json_object_get_members (elements);
						for (GList *l = members; l != NULL; l = l->next) {
							const gchar *key = l->data;
							JsonObject *elem = json_object_get_object_member (elements, key);
							if (json_object_has_member (elem, "storePaths")) {
								JsonArray *paths = json_object_get_array_member (elem, "storePaths");
								if (json_array_get_length (paths) > 0) {
									const gchar *path = json_array_get_string_element (paths, 0);
									const gchar *base = g_strrstr (path, "/");
									if (base != NULL) {
										base++;
										if (strlen (base) > 33) {
											g_autofree gchar *name_ver = g_strdup (base + 33);
											gchar *dash = strchr (name_ver, '-');
											while (dash != NULL && !g_ascii_isdigit (dash[1])) {
												dash = strchr (dash + 1, '-');
											}
											if (dash != NULL) {
												*dash = '\0';
											}
											if (g_strcmp0 (name_ver, package_name) == 0) {
												index_str = g_strdup (key);
												break;
											}
										}
									}
								}
							}
						}
						g_list_free (members);
					}
				}
			}
		}
	}
	return index_str;
}

static void
merge_declarative_and_system_packages (GsPlugin *plugin, GsAppList *merged)
{
	GsPluginNixos *self = GS_PLUGIN_NIXOS (plugin);
	g_autoptr(GError) sys_error = NULL;
	g_autoptr(GsAppList) sys_list = NULL;
	g_autoptr(GError) user_error = NULL;
	g_autoptr(GsAppList) user_list = NULL;
	g_autoptr(GError) path_error = NULL;
	g_autoptr(GsAppList) sys_path_list = NULL;
	guint i;

	/* 1. System declarative (configuration.nix) */
	if (self->system_backend == GS_NIXOS_BACKEND_DECLARATIVE &&
	    self->declarative_system_file != NULL) {
		sys_list = list_declarative_packages (plugin, self->declarative_system_file, &sys_error);
		if (sys_list != NULL) {
			for (i = 0; i < gs_app_list_length (sys_list); i++) {
				GsApp *a = gs_app_list_index (sys_list, i);
				gs_app_set_metadata (a, "NixOS::install-source", "system");
				if (gs_app_list_lookup (merged, gs_app_get_unique_id (a)) == NULL)
					gs_app_list_add (merged, a);
			}
		}
	}

	/* 2. User declarative (home-manager) */
	if (self->user_backend == GS_NIXOS_BACKEND_DECLARATIVE &&
	    self->declarative_user_file != NULL) {
		user_list = list_declarative_packages (plugin, self->declarative_user_file, &user_error);
		if (user_list != NULL) {
			for (i = 0; i < gs_app_list_length (user_list); i++) {
				GsApp *a = gs_app_list_index (user_list, i);
				gs_app_set_metadata (a, "NixOS::install-source", "user");
				if (gs_app_list_lookup (merged, gs_app_get_unique_id (a)) == NULL)
					gs_app_list_add (merged, a);
			}
		}
	}

	/* 3. System path fallback — always add packages from /run/current-system/sw */
	if (g_file_test ("/run/current-system/sw/bin", G_FILE_TEST_IS_DIR)) {
		sys_path_list = list_system_path_packages (plugin, &path_error);
		if (sys_path_list != NULL) {
			for (i = 0; i < gs_app_list_length (sys_path_list); i++) {
				GsApp *a = gs_app_list_index (sys_path_list, i);
				/* Don't duplicate packages already from declarative system */
				if (gs_app_list_lookup (merged, gs_app_get_unique_id (a)) == NULL)
					gs_app_list_add (merged, a);
			}
		}
	}
}

static void list_apps_communicate_cb (GObject *source_object, GAsyncResult *result, gpointer user_data);

static void
gs_plugin_nixos_list_apps_async (GsPlugin              *plugin,
                                 GsAppQuery            *query,
                                 GsPluginListAppsFlags  flags,
                                 GsPluginEventCallback  event_callback,
                                 void                  *event_user_data,
                                 GCancellable          *cancellable,
                                 GAsyncReadyCallback    callback,
                                 gpointer               user_data)
{
	GsPluginNixos *self = GS_PLUGIN_NIXOS (plugin);
	g_autoptr(GTask) task = gs_plugin_list_apps_data_new_task (plugin, query, flags, event_callback, event_user_data, cancellable, callback, user_data);
	GsApp *alternate_of = NULL;
	GsAppQueryTristate is_installed;
	g_autoptr(GSubprocess) subprocess = NULL;
	g_autoptr(GError) local_error = NULL;
	ListAppsData *data = NULL;
	g_autoptr(GsAppList) merged = NULL;

	g_task_set_source_tag (task, gs_plugin_nixos_list_apps_async);

	if (query == NULL) {
		g_task_return_pointer (task, gs_app_list_new (), g_object_unref);
		return;
	}

	alternate_of = gs_app_query_get_alternate_of (query);
	if (alternate_of != NULL) {
		g_autoptr(GsAppList) list = gs_app_list_new ();
		const gchar *app_id = gs_app_get_id (alternate_of);
		if (app_id != NULL) {
			gchar *pkg_name = guess_nix_package_name (alternate_of);
			if (pkg_name != NULL) {
				g_autoptr(GsApp) app = gs_app_new (app_id);
				gs_app_set_management_plugin (app, plugin);
				gs_app_set_kind (app, gs_app_get_kind (alternate_of));
				gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_PACKAGE);
				gs_app_add_source (app, pkg_name);
				gs_app_set_origin (app, "nixpkgs");
				gs_app_set_origin_ui (app, "NixOS");
				gs_app_set_metadata (app, "GnomeSoftware::PackagingFormat", "Nix");
				gs_app_set_metadata (app, "GnomeSoftware::PackagingBaseCssColor", "accent_color");

				gboolean installed = is_package_installed (self, pkg_name);
				if (installed) {
					gs_app_set_state (app, GS_APP_STATE_INSTALLED);
				} else {
					gs_app_set_state (app, GS_APP_STATE_AVAILABLE);
				}
				gs_app_list_add (list, app);
				g_task_return_pointer (task, g_steal_pointer (&list), g_object_unref);
				g_free (pkg_name);
				return;
			}
		}
		g_task_return_pointer (task, gs_app_list_new (), g_object_unref);
		return;
	}

	is_installed = gs_app_query_get_is_installed (query);
	if (is_installed != GS_APP_QUERY_TRISTATE_TRUE) {
		g_task_return_pointer (task, gs_app_list_new (), g_object_unref);
		return;
	}

	/* If we need user backend profile/env, query asynchronously */
	if (self->user_backend == GS_NIXOS_BACKEND_PROFILE || self->user_backend == GS_NIXOS_BACKEND_ENV) {
		if (self->user_backend == GS_NIXOS_BACKEND_ENV) {
			subprocess = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
			                               &local_error,
			                               "nix-env", "-q", "--json", NULL);
		} else {
			subprocess = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
			                               &local_error,
			                               "nix", "profile", "list", "--json", NULL);
		}

		if (subprocess == NULL) {
			g_task_return_error (task, g_steal_pointer (&local_error));
			return;
		}

		data = g_slice_new0 (ListAppsData);
		data->subprocess = g_object_ref (subprocess);
		data->self = self;

		g_task_set_task_data (task, data, (GDestroyNotify) list_apps_data_free);
		g_subprocess_communicate_async (subprocess, NULL, cancellable, list_apps_communicate_cb, g_steal_pointer (&task));
	} else {
		/* Declarative or none user backend, list synchronously and return */
		merged = gs_app_list_new ();
		merge_declarative_and_system_packages (plugin, merged);
		g_task_return_pointer (task, g_steal_pointer (&merged), g_object_unref);
	}
}

static void
list_apps_communicate_cb (GObject *source_object, GAsyncResult *result, gpointer user_data)
{
	GSubprocess *subprocess = G_SUBPROCESS (source_object);
	g_autoptr(GTask) task = G_TASK (user_data);
	ListAppsData *data = g_task_get_task_data (task);
	g_autoptr(GError) local_error = NULL;
	g_autoptr(GBytes) stdout_buf = NULL;
	const gchar *output = NULL;
	g_autoptr(GsAppList) list = NULL;

	if (!g_subprocess_communicate_finish (subprocess, result, &stdout_buf, NULL, &local_error)) {
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	output = g_bytes_get_data (stdout_buf, NULL);
	if (output == NULL) output = "";

	if (data->self->user_backend == GS_NIXOS_BACKEND_PROFILE) {
		list = parse_nix_profile_json (GS_PLUGIN (data->self), output, &local_error);
	} else {
		list = parse_nix_env_json (GS_PLUGIN (data->self), output, &local_error);
	}

	if (list == NULL) {
		g_task_return_error (task, g_steal_pointer (&local_error));
	} else {
		/* Merge with declarative & system path packages */
		merge_declarative_and_system_packages (GS_PLUGIN (data->self), list);
		g_task_return_pointer (task, g_steal_pointer (&list), g_object_unref);
	}
}

static GsAppList *
gs_plugin_nixos_list_apps_finish (GsPlugin *plugin, GAsyncResult *result, GError **error)
{
	return g_task_propagate_pointer (G_TASK (result), error);
}

static void
command_communicate_cb (GObject *source_object, GAsyncResult *result, gpointer user_data)
{
	GSubprocess *subprocess = G_SUBPROCESS (source_object);
	g_autoptr(GTask) task = G_TASK (user_data);
	GsPluginNixos *self = GS_PLUGIN_NIXOS (g_task_get_source_object (task));
	g_autoptr(GError) local_error = NULL;
	g_autoptr(GBytes) stderr_buf = NULL;

	self->in_plugin_action = FALSE;

	if (!g_subprocess_communicate_finish (subprocess, result, NULL, &stderr_buf, &local_error)) {
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	gboolean success = g_subprocess_get_successful (subprocess);
	if (!success) {
		const gchar *err_msg = stderr_buf ? g_bytes_get_data (stderr_buf, NULL) : "Unknown error";
		g_task_return_new_error (task, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
		                         "Nix command failed: %s", err_msg);
	} else {
		g_task_return_boolean (task, TRUE);
	}
}

static void
gs_plugin_nixos_install_apps_async (GsPlugin                           *plugin,
                                    GsAppList                          *apps,
                                    GsPluginInstallAppsFlags            flags,
                                    GsPluginProgressCallback            progress_callback,
                                    gpointer                            progress_user_data,
                                    GsPluginEventCallback               event_callback,
                                    void                               *event_user_data,
                                    GsPluginAppNeedsUserActionCallback  app_needs_user_action_callback,
                                    gpointer                            app_needs_user_action_data,
                                    GCancellable                       *cancellable,
                                    GAsyncReadyCallback                 callback,
                                    gpointer                            user_data)
{
	GsPluginNixos *self = GS_PLUGIN_NIXOS (plugin);
	g_autoptr(GTask) task = g_task_new (plugin, cancellable, callback, user_data);
	g_task_set_source_tag (task, gs_plugin_nixos_install_apps_async);

	self->in_plugin_action = TRUE;
	load_config (self);

	g_autoptr(GSettings) settings = g_settings_new ("org.gnome.software");

	/* Handle special NixOS options (steam, docker…) */
	for (guint i = 0; i < gs_app_list_length (apps); i++) {
		GsApp *app = gs_app_list_index (apps, i);
		g_autofree gchar *pkg_name = guess_nix_package_name (app);
		for (guint j = 0; j < G_N_ELEMENTS (special_options); j++) {
			if (g_strcmp0 (pkg_name, special_options[j].package_name) == 0) {
				g_autofree gchar *key = g_strdup_printf ("nixos-option-%s", pkg_name);
				g_settings_set_boolean (settings, key, TRUE);
				break;
			}
		}
	}

	for (guint i = 0; i < gs_app_list_length (apps); i++) {
		GsApp *app = gs_app_list_index (apps, i);
		gs_app_set_state (app, GS_APP_STATE_INSTALLING);
	}

	/* Determine effective install source for each app.
	 * Apps carry NixOS::install-source set by list_apps or by the source dialog.
	 * If absent and only one backend is active, use it; otherwise default to profile. */
	gboolean has_system = (self->system_backend == GS_NIXOS_BACKEND_DECLARATIVE);
	gboolean has_user   = (self->user_backend   == GS_NIXOS_BACKEND_DECLARATIVE);
	gboolean has_profile = (self->user_backend  == GS_NIXOS_BACKEND_PROFILE);
	gboolean has_env    = (self->user_backend   == GS_NIXOS_BACKEND_ENV);

	/* Group apps per install target */
	g_autoptr(GsAppList) system_apps  = gs_app_list_new ();
	g_autoptr(GsAppList) user_apps    = gs_app_list_new ();
	g_autoptr(GsAppList) profile_apps = gs_app_list_new ();
	g_autoptr(GsAppList) env_apps     = gs_app_list_new ();

	for (guint i = 0; i < gs_app_list_length (apps); i++) {
		GsApp *app = gs_app_list_index (apps, i);
		const gchar *src = gs_app_get_metadata_item (app, "NixOS::install-source");

		if (g_strcmp0 (src, "system") == 0 && has_system)
			gs_app_list_add (system_apps, app);
		else if (g_strcmp0 (src, "user") == 0 && has_user)
			gs_app_list_add (user_apps, app);
		else if (g_strcmp0 (src, "env") == 0 || has_env)
			gs_app_list_add (env_apps, app);
		else if (has_profile)
			gs_app_list_add (profile_apps, app);
		else if (has_system)
			gs_app_list_add (system_apps, app);
		else
			gs_app_list_add (profile_apps, app); /* safest fallback */
	}

	/* --- Install system apps (declarative + nixos-rebuild) --- */
	if (gs_app_list_length (system_apps) > 0) {
		g_autoptr(GError) local_error = NULL;
		g_autoptr(GPtrArray) packages = parse_nix_file_packages (self->declarative_system_file);
		for (guint i = 0; i < gs_app_list_length (system_apps); i++) {
			GsApp *app = gs_app_list_index (system_apps, i);
			g_autofree gchar *pkg_name = guess_nix_package_name (app);
			gboolean found = FALSE;
			for (guint j = 0; j < packages->len; j++) {
				if (g_strcmp0 (g_ptr_array_index (packages, j), pkg_name) == 0) {
					found = TRUE; break;
				}
			}
			if (!found)
				g_ptr_array_add (packages, g_strdup (pkg_name));
		}

		const gchar *tmp_file = "/tmp/gnome-software-system-packages.nix.tmp";
		if (!write_nix_file_packages (tmp_file, packages, FALSE, &local_error)) {
			for (guint i = 0; i < gs_app_list_length (system_apps); i++)
				gs_app_set_state (gs_app_list_index (system_apps, i), GS_APP_STATE_AVAILABLE);
			g_task_return_error (task, g_steal_pointer (&local_error));
			return;
		}

		g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func (g_free);
		g_ptr_array_add (argv, g_strdup ("pkexec"));
		g_ptr_array_add (argv, g_strdup ("sh"));
		g_ptr_array_add (argv, g_strdup ("-c"));
		if (self->use_flakes) {
			g_ptr_array_add (argv, g_strdup_printf ("mv %s %s && nixos-rebuild switch --flake %s",
			                                        tmp_file, self->declarative_system_file, self->system_flake_dir));
		} else {
			g_ptr_array_add (argv, g_strdup_printf ("mv %s %s && nixos-rebuild switch",
			                                        tmp_file, self->declarative_system_file));
		}
		g_ptr_array_add (argv, NULL);

		g_autoptr(GSubprocess) subprocess = g_subprocess_newv (
			(const gchar * const *) argv->pdata,
			G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
			&local_error);
		if (subprocess != NULL)
			g_subprocess_communicate (subprocess, NULL, NULL, NULL, NULL, NULL);
	}

	/* --- Install user apps (home-manager declarative) --- */
	if (gs_app_list_length (user_apps) > 0) {
		g_autoptr(GError) local_error = NULL;
		g_autoptr(GPtrArray) packages = parse_nix_file_packages (self->declarative_user_file);
		for (guint i = 0; i < gs_app_list_length (user_apps); i++) {
			GsApp *app = gs_app_list_index (user_apps, i);
			g_autofree gchar *pkg_name = guess_nix_package_name (app);
			gboolean found = FALSE;
			for (guint j = 0; j < packages->len; j++) {
				if (g_strcmp0 (g_ptr_array_index (packages, j), pkg_name) == 0) {
					found = TRUE; break;
				}
			}
			if (!found)
				g_ptr_array_add (packages, g_strdup (pkg_name));
		}

		if (!write_nix_file_packages (self->declarative_user_file, packages, TRUE, &local_error)) {
			for (guint i = 0; i < gs_app_list_length (user_apps); i++)
				gs_app_set_state (gs_app_list_index (user_apps, i), GS_APP_STATE_AVAILABLE);
			g_task_return_error (task, g_steal_pointer (&local_error));
			return;
		}

		g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func (g_free);
		g_ptr_array_add (argv, g_strdup ("home-manager"));
		g_ptr_array_add (argv, g_strdup ("switch"));
		if (self->use_flakes && self->home_manager_flake_dir != NULL) {
			g_ptr_array_add (argv, g_strdup ("--flake"));
			g_ptr_array_add (argv, g_strdup (self->home_manager_flake_dir));
		}
		g_ptr_array_add (argv, NULL);

		g_autoptr(GSubprocess) subprocess = g_subprocess_newv (
			(const gchar * const *) argv->pdata,
			G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
			NULL);
		if (subprocess != NULL)
			g_subprocess_communicate (subprocess, NULL, NULL, NULL, NULL, NULL);
	}

	/* --- Install via nix profile --- */
	if (gs_app_list_length (profile_apps) > 0) {
		g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func (g_free);
		g_ptr_array_add (argv, g_strdup ("nix"));
		g_ptr_array_add (argv, g_strdup ("profile"));
		g_ptr_array_add (argv, g_strdup ("install"));
		for (guint i = 0; i < gs_app_list_length (profile_apps); i++) {
			GsApp *app = gs_app_list_index (profile_apps, i);
			g_autofree gchar *pkg_name = guess_nix_package_name (app);
			g_ptr_array_add (argv, g_strdup_printf ("%s#%s", self->flake_uri, pkg_name));
		}
		g_ptr_array_add (argv, NULL);

		g_autoptr(GError) local_error = NULL;
		g_autoptr(GSubprocess) subprocess = g_subprocess_newv (
			(const gchar * const *) argv->pdata,
			G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
			&local_error);
		if (subprocess == NULL) {
			for (guint i = 0; i < gs_app_list_length (profile_apps); i++)
				gs_app_set_state (gs_app_list_index (profile_apps, i), GS_APP_STATE_AVAILABLE);
			g_task_return_error (task, g_steal_pointer (&local_error));
			return;
		}

		g_task_set_task_data (task, g_object_ref (subprocess), g_object_unref);
		g_subprocess_communicate_async (subprocess, NULL, cancellable, command_communicate_cb, g_steal_pointer (&task));
		return;
	}

	/* --- Install via nix-env --- */
	if (gs_app_list_length (env_apps) > 0) {
		g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func (g_free);
		g_ptr_array_add (argv, g_strdup ("nix-env"));
		g_ptr_array_add (argv, g_strdup ("-iA"));
		for (guint i = 0; i < gs_app_list_length (env_apps); i++) {
			GsApp *app = gs_app_list_index (env_apps, i);
			g_autofree gchar *pkg_name = guess_nix_package_name (app);
			g_ptr_array_add (argv, g_strdup_printf ("nixpkgs.%s", pkg_name));
		}
		g_ptr_array_add (argv, NULL);

		g_autoptr(GError) local_error = NULL;
		g_autoptr(GSubprocess) subprocess = g_subprocess_newv (
			(const gchar * const *) argv->pdata,
			G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
			&local_error);
		if (subprocess == NULL) {
			for (guint i = 0; i < gs_app_list_length (env_apps); i++)
				gs_app_set_state (gs_app_list_index (env_apps, i), GS_APP_STATE_AVAILABLE);
			g_task_return_error (task, g_steal_pointer (&local_error));
			return;
		}

		g_task_set_task_data (task, g_object_ref (subprocess), g_object_unref);
		g_subprocess_communicate_async (subprocess, NULL, cancellable, command_communicate_cb, g_steal_pointer (&task));
		return;
	}

	/* All done (synchronous paths above) */
	for (guint i = 0; i < gs_app_list_length (apps); i++)
		gs_app_set_state (gs_app_list_index (apps, i), GS_APP_STATE_INSTALLED);
	g_task_return_boolean (task, TRUE);
}

static gboolean
gs_plugin_nixos_install_apps_finish (GsPlugin *plugin, GAsyncResult *result, GError **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
}

typedef struct {
	GsPluginNixos *self;
	GTask *task;
	GCancellable *cancellable;
	GsAppList *apps;

	GsAppList *system_apps;
	GsAppList *user_apps;
	GsAppList *profile_apps;
	GsAppList *env_apps;

	gint step;
} UninstallState;

static void
uninstall_state_free (UninstallState *state)
{
	g_clear_object (&state->task);
	g_clear_object (&state->cancellable);
	g_clear_object (&state->apps);
	g_clear_object (&state->system_apps);
	g_clear_object (&state->user_apps);
	g_clear_object (&state->profile_apps);
	g_clear_object (&state->env_apps);
	g_slice_free (UninstallState, state);
}

static void uninstall_next_step (UninstallState *state);

static void
uninstall_command_communicate_cb (GObject *source_object, GAsyncResult *result, gpointer user_data)
{
	GSubprocess *subprocess = G_SUBPROCESS (source_object);
	UninstallState *state = (UninstallState *) user_data;
	g_autoptr(GError) local_error = NULL;
	g_autoptr(GBytes) stderr_buf = NULL;
	guint i;

	if (!g_subprocess_communicate_finish (subprocess, result, NULL, &stderr_buf, &local_error)) {
		state->self->in_plugin_action = FALSE;
		for (i = 0; i < gs_app_list_length (state->apps); i++) {
			GsApp *app = gs_app_list_index (state->apps, i);
			gs_app_set_state (app, GS_APP_STATE_INSTALLED);
		}
		g_task_return_error (state->task, g_steal_pointer (&local_error));
		uninstall_state_free (state);
		return;
	}

	if (!g_subprocess_get_successful (subprocess)) {
		const gchar *err_msg = stderr_buf ? g_bytes_get_data (stderr_buf, NULL) : "Unknown error";
		state->self->in_plugin_action = FALSE;
		for (i = 0; i < gs_app_list_length (state->apps); i++) {
			GsApp *app = gs_app_list_index (state->apps, i);
			gs_app_set_state (app, GS_APP_STATE_INSTALLED);
		}
		g_task_return_new_error (state->task, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
		                         "Nix command failed: %s", err_msg);
		uninstall_state_free (state);
		return;
	}

	state->step++;
	uninstall_next_step (state);
}

static void
uninstall_next_step (UninstallState *state)
{
	GsPluginNixos *self = state->self;
	guint i;

	if (state->step == 0) {
		/* Step 0: System declarative apps */
		if (gs_app_list_length (state->system_apps) > 0) {
			g_autoptr(GPtrArray) packages = NULL;
			g_autoptr(GPtrArray) argv = NULL;
			g_autoptr(GError) local_error = NULL;
			const gchar *tmp_file = "/tmp/gnome-software-packages.nix.tmp";
			g_autofree gchar *flake_nix = NULL;
			gboolean use_flake = FALSE;
			g_autoptr(GSubprocess) subprocess = NULL;
			guint j;

			packages = parse_nix_file_packages (self->declarative_system_file);
			argv = g_ptr_array_new_with_free_func (g_free);
			flake_nix = g_build_filename (self->system_flake_dir, "flake.nix", NULL);
			use_flake = g_file_test (flake_nix, G_FILE_TEST_EXISTS);

			for (i = 0; i < gs_app_list_length (state->system_apps); i++) {
				GsApp *app = gs_app_list_index (state->system_apps, i);
				g_autofree gchar *pkg_name = guess_nix_package_name (app);
				for (j = 0; j < packages->len; j++) {
					if (g_strcmp0 (g_ptr_array_index (packages, j), pkg_name) == 0) {
						g_ptr_array_remove_index (packages, j);
						break;
					}
				}
			}

			if (!write_nix_file_packages (tmp_file, packages, FALSE, &local_error)) {
				self->in_plugin_action = FALSE;
				for (i = 0; i < gs_app_list_length (state->apps); i++) {
					GsApp *app = gs_app_list_index (state->apps, i);
					gs_app_set_state (app, GS_APP_STATE_INSTALLED);
				}
				g_task_return_error (state->task, g_steal_pointer (&local_error));
				uninstall_state_free (state);
				return;
			}

			g_ptr_array_add (argv, g_strdup ("pkexec"));
			g_ptr_array_add (argv, g_strdup ("sh"));
			g_ptr_array_add (argv, g_strdup ("-c"));
			if (use_flake) {
				g_ptr_array_add (argv, g_strdup_printf ("mv %s %s && nixos-rebuild switch --flake %s",
				                                        tmp_file, self->declarative_system_file, self->system_flake_dir));
			} else {
				g_ptr_array_add (argv, g_strdup_printf ("mv %s %s && nixos-rebuild switch",
				                                        tmp_file, self->declarative_system_file));
			}
			g_ptr_array_add (argv, NULL);

			subprocess = g_subprocess_newv ((const gchar * const *) argv->pdata,
			                                G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
			                                &local_error);
			if (subprocess == NULL) {
				self->in_plugin_action = FALSE;
				for (i = 0; i < gs_app_list_length (state->apps); i++) {
					GsApp *app = gs_app_list_index (state->apps, i);
					gs_app_set_state (app, GS_APP_STATE_INSTALLED);
				}
				g_task_return_error (state->task, g_steal_pointer (&local_error));
				uninstall_state_free (state);
				return;
			}

			g_subprocess_communicate_async (subprocess, NULL, state->cancellable, uninstall_command_communicate_cb, state);
			return;
		}
		state->step++;
	}

	if (state->step == 1) {
		/* Step 1: User declarative apps */
		if (gs_app_list_length (state->user_apps) > 0) {
			g_autoptr(GPtrArray) packages = NULL;
			g_autoptr(GPtrArray) argv = NULL;
			g_autoptr(GError) local_error = NULL;
			g_autoptr(GSubprocess) subprocess = NULL;
			guint j;

			packages = parse_nix_file_packages (self->declarative_user_file);
			argv = g_ptr_array_new_with_free_func (g_free);

			for (i = 0; i < gs_app_list_length (state->user_apps); i++) {
				GsApp *app = gs_app_list_index (state->user_apps, i);
				g_autofree gchar *pkg_name = guess_nix_package_name (app);
				for (j = 0; j < packages->len; j++) {
					if (g_strcmp0 (g_ptr_array_index (packages, j), pkg_name) == 0) {
						g_ptr_array_remove_index (packages, j);
						break;
					}
				}
			}

			if (!write_nix_file_packages (self->declarative_user_file, packages, TRUE, &local_error)) {
				self->in_plugin_action = FALSE;
				for (i = 0; i < gs_app_list_length (state->apps); i++) {
					GsApp *app = gs_app_list_index (state->apps, i);
					gs_app_set_state (app, GS_APP_STATE_INSTALLED);
				}
				g_task_return_error (state->task, g_steal_pointer (&local_error));
				uninstall_state_free (state);
				return;
			}

			g_ptr_array_add (argv, g_strdup ("home-manager"));
			g_ptr_array_add (argv, g_strdup ("switch"));
			if (self->use_flakes && self->home_manager_flake_dir != NULL) {
				g_ptr_array_add (argv, g_strdup ("--flake"));
				g_ptr_array_add (argv, g_strdup (self->home_manager_flake_dir));
			}
			g_ptr_array_add (argv, NULL);

			subprocess = g_subprocess_newv ((const gchar * const *) argv->pdata,
			                                G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
			                                &local_error);
			if (subprocess == NULL) {
				self->in_plugin_action = FALSE;
				for (i = 0; i < gs_app_list_length (state->apps); i++) {
					GsApp *app = gs_app_list_index (state->apps, i);
					gs_app_set_state (app, GS_APP_STATE_INSTALLED);
				}
				g_task_return_error (state->task, g_steal_pointer (&local_error));
				uninstall_state_free (state);
				return;
			}

			g_subprocess_communicate_async (subprocess, NULL, state->cancellable, uninstall_command_communicate_cb, state);
			return;
		}
		state->step++;
	}

	if (state->step == 2) {
		/* Step 2: Nix profile apps */
		if (gs_app_list_length (state->profile_apps) > 0) {
			g_autoptr(GPtrArray) argv = NULL;
			g_autoptr(GError) local_error = NULL;
			g_autoptr(GSubprocess) subprocess = NULL;

			argv = g_ptr_array_new_with_free_func (g_free);
			g_ptr_array_add (argv, g_strdup ("nix"));
			g_ptr_array_add (argv, g_strdup ("profile"));
			g_ptr_array_add (argv, g_strdup ("remove"));
			for (i = 0; i < gs_app_list_length (state->profile_apps); i++) {
				GsApp *app = gs_app_list_index (state->profile_apps, i);
				const gchar *name = gs_app_get_id (app);
				gboolean is_numeric = TRUE;
				const gchar *p;
				for (p = name; p != NULL && *p != '\0'; p++) {
					if (!g_ascii_isdigit (*p)) {
						is_numeric = FALSE;
						break;
					}
				}
				if (is_numeric) {
					g_ptr_array_add (argv, g_strdup (name));
				} else {
					g_autofree gchar *pkg_name = guess_nix_package_name (app);
					gchar *idx = get_nix_profile_index_by_package_name (self, pkg_name);
					if (idx != NULL) {
						g_ptr_array_add (argv, idx);
					} else {
						g_ptr_array_add (argv, g_strdup (pkg_name));
					}
				}
			}
			g_ptr_array_add (argv, NULL);

			subprocess = g_subprocess_newv ((const gchar * const *) argv->pdata,
			                                G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
			                                &local_error);
			if (subprocess == NULL) {
				self->in_plugin_action = FALSE;
				for (i = 0; i < gs_app_list_length (state->apps); i++) {
					GsApp *app = gs_app_list_index (state->apps, i);
					gs_app_set_state (app, GS_APP_STATE_INSTALLED);
				}
				g_task_return_error (state->task, g_steal_pointer (&local_error));
				uninstall_state_free (state);
				return;
			}

			g_subprocess_communicate_async (subprocess, NULL, state->cancellable, uninstall_command_communicate_cb, state);
			return;
		}
		state->step++;
	}

	if (state->step == 3) {
		/* Step 3: Nix env apps */
		if (gs_app_list_length (state->env_apps) > 0) {
			g_autoptr(GPtrArray) argv = NULL;
			g_autoptr(GError) local_error = NULL;
			g_autoptr(GSubprocess) subprocess = NULL;

			argv = g_ptr_array_new_with_free_func (g_free);
			g_ptr_array_add (argv, g_strdup ("nix-env"));
			g_ptr_array_add (argv, g_strdup ("-e"));
			for (i = 0; i < gs_app_list_length (state->env_apps); i++) {
				GsApp *app = gs_app_list_index (state->env_apps, i);
				g_autofree gchar *pkg_name = guess_nix_package_name (app);
				g_ptr_array_add (argv, g_strdup (pkg_name));
			}
			g_ptr_array_add (argv, NULL);

			subprocess = g_subprocess_newv ((const gchar * const *) argv->pdata,
			                                G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
			                                &local_error);
			if (subprocess == NULL) {
				self->in_plugin_action = FALSE;
				for (i = 0; i < gs_app_list_length (state->apps); i++) {
					GsApp *app = gs_app_list_index (state->apps, i);
					gs_app_set_state (app, GS_APP_STATE_INSTALLED);
				}
				g_task_return_error (state->task, g_steal_pointer (&local_error));
				uninstall_state_free (state);
				return;
			}

			g_subprocess_communicate_async (subprocess, NULL, state->cancellable, uninstall_command_communicate_cb, state);
			return;
		}
		state->step++;
	}

	/* All steps completed successfully */
	self->in_plugin_action = FALSE;
	for (i = 0; i < gs_app_list_length (state->apps); i++) {
		GsApp *app = gs_app_list_index (state->apps, i);
		gs_app_set_state (app, GS_APP_STATE_AVAILABLE);
	}
	g_task_return_boolean (state->task, TRUE);
	uninstall_state_free (state);
}

static void
gs_plugin_nixos_uninstall_apps_async (GsPlugin                             *plugin,
                                      GsAppList                            *apps,
                                      GsPluginUninstallAppsFlags            flags,
                                      GsPluginProgressCallback              progress_callback,
                                      gpointer                              progress_user_data,
                                      GsPluginEventCallback                 event_callback,
                                      void                                 *event_user_data,
                                      GsPluginAppNeedsUserActionCallback    app_needs_user_action_callback,
                                      gpointer                              app_needs_user_action_data,
                                      GCancellable                         *cancellable,
                                      GAsyncReadyCallback                   callback,
                                      gpointer                              user_data)
{
	GsPluginNixos *self = GS_PLUGIN_NIXOS (plugin);
	g_autoptr(GTask) task = g_task_new (plugin, cancellable, callback, user_data);
	g_autoptr(GSettings) settings = NULL;
	UninstallState *state = NULL;
	gboolean has_system;
	gboolean has_user;
	gboolean has_profile;
	gboolean has_env;
	guint i;

	g_task_set_source_tag (task, gs_plugin_nixos_uninstall_apps_async);

	self->in_plugin_action = TRUE;
	load_config (self);

	settings = g_settings_new ("org.gnome.software");
	for (i = 0; i < gs_app_list_length (apps); i++) {
		GsApp *app = gs_app_list_index (apps, i);
		g_autofree gchar *pkg_name = guess_nix_package_name (app);
		guint j;
		for (j = 0; j < G_N_ELEMENTS (special_options); j++) {
			if (g_strcmp0 (pkg_name, special_options[j].package_name) == 0) {
				g_autofree gchar *key = g_strdup_printf ("nixos-option-%s", pkg_name);
				g_settings_set_boolean (settings, key, FALSE);
				break;
			}
		}
	}

	for (i = 0; i < gs_app_list_length (apps); i++) {
		GsApp *app = gs_app_list_index (apps, i);
		gs_app_set_state (app, GS_APP_STATE_REMOVING);
	}

	has_system = (self->system_backend == GS_NIXOS_BACKEND_DECLARATIVE);
	has_user   = (self->user_backend   == GS_NIXOS_BACKEND_DECLARATIVE);
	has_profile = (self->user_backend  == GS_NIXOS_BACKEND_PROFILE);
	has_env    = (self->user_backend   == GS_NIXOS_BACKEND_ENV);

	state = g_slice_new0 (UninstallState);
	state->self = self;
	state->task = g_object_ref (task);
	state->cancellable = (cancellable != NULL) ? g_object_ref (cancellable) : NULL;
	state->apps = g_object_ref (apps);
	state->system_apps = gs_app_list_new ();
	state->user_apps = gs_app_list_new ();
	state->profile_apps = gs_app_list_new ();
	state->env_apps = gs_app_list_new ();
	state->step = 0;

	for (i = 0; i < gs_app_list_length (apps); i++) {
		GsApp *app = gs_app_list_index (apps, i);
		g_autofree gchar *pkg_name = guess_nix_package_name (app);
		const gchar *src = gs_app_get_metadata_item (app, "NixOS::install-source");
		if (src == NULL) {
			src = detect_package_install_source (self, pkg_name);
		}

		if (g_strcmp0 (src, "system") == 0 && has_system)
			gs_app_list_add (state->system_apps, app);
		else if (g_strcmp0 (src, "user") == 0 && has_user)
			gs_app_list_add (state->user_apps, app);
		else if (g_strcmp0 (src, "env") == 0 || has_env)
			gs_app_list_add (state->env_apps, app);
		else if (has_profile)
			gs_app_list_add (state->profile_apps, app);
		else if (has_system)
			gs_app_list_add (state->system_apps, app);
		else
			gs_app_list_add (state->profile_apps, app);
	}

	uninstall_next_step (state);
}

static gboolean
gs_plugin_nixos_uninstall_apps_finish (GsPlugin *plugin, GAsyncResult *result, GError **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
}

typedef struct {
	GsPluginNixos *self;
	GTask *task;
	GCancellable *cancellable;
	GsAppList *apps;

	GsAppList *system_apps;
	GsAppList *user_apps;
	GsAppList *profile_apps;
	GsAppList *env_apps;

	gint step;
} UpdateState;

static void
update_state_free (UpdateState *state)
{
	g_clear_object (&state->task);
	g_clear_object (&state->cancellable);
	g_clear_object (&state->apps);
	g_clear_object (&state->system_apps);
	g_clear_object (&state->user_apps);
	g_clear_object (&state->profile_apps);
	g_clear_object (&state->env_apps);
	g_slice_free (UpdateState, state);
}

static void update_next_step (UpdateState *state);

static void
update_command_communicate_cb (GObject *source_object, GAsyncResult *result, gpointer user_data)
{
	GSubprocess *subprocess = G_SUBPROCESS (source_object);
	UpdateState *state = (UpdateState *) user_data;
	g_autoptr(GError) local_error = NULL;
	g_autoptr(GBytes) stderr_buf = NULL;
	guint i;

	if (!g_subprocess_communicate_finish (subprocess, result, NULL, &stderr_buf, &local_error)) {
		state->self->in_plugin_action = FALSE;
		for (i = 0; i < gs_app_list_length (state->apps); i++) {
			GsApp *app = gs_app_list_index (state->apps, i);
			gs_app_set_state (app, GS_APP_STATE_UPDATABLE);
		}
		g_task_return_error (state->task, g_steal_pointer (&local_error));
		update_state_free (state);
		return;
	}

	if (!g_subprocess_get_successful (subprocess)) {
		const gchar *err_msg = stderr_buf ? g_bytes_get_data (stderr_buf, NULL) : "Unknown error";
		state->self->in_plugin_action = FALSE;
		for (i = 0; i < gs_app_list_length (state->apps); i++) {
			GsApp *app = gs_app_list_index (state->apps, i);
			gs_app_set_state (app, GS_APP_STATE_UPDATABLE);
		}
		g_task_return_new_error (state->task, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
		                         "Nix command failed: %s", err_msg);
		update_state_free (state);
		return;
	}

	state->step++;
	update_next_step (state);
}

static void
update_next_step (UpdateState *state)
{
	GsPluginNixos *self = state->self;
	guint i;

	if (state->step == 0) {
		/* Step 0: System declarative apps update */
		gboolean run_system = (self->system_backend == GS_NIXOS_BACKEND_DECLARATIVE) &&
		                      (gs_app_list_length (state->apps) == 0 || gs_app_list_length (state->system_apps) > 0);

		if (run_system) {
			g_autofree gchar *flake_nix = g_build_filename (self->system_flake_dir, "flake.nix", NULL);
			gboolean use_flake = g_file_test (flake_nix, G_FILE_TEST_EXISTS);
			g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func (g_free);
			g_autoptr(GError) local_error = NULL;
			g_autoptr(GSubprocess) subprocess = NULL;

			g_ptr_array_add (argv, g_strdup ("pkexec"));
			g_ptr_array_add (argv, g_strdup ("sh"));
			g_ptr_array_add (argv, g_strdup ("-c"));
			if (use_flake) {
				g_ptr_array_add (argv, g_strdup_printf ("nix flake update --flake %s && nixos-rebuild switch --flake %s",
				                                        self->system_flake_dir, self->system_flake_dir));
			} else {
				g_ptr_array_add (argv, g_strdup ("nix-channel --update nixos && nixos-rebuild switch"));
			}
			g_ptr_array_add (argv, NULL);

			subprocess = g_subprocess_newv ((const gchar * const *) argv->pdata,
			                                G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
			                                &local_error);
			if (subprocess == NULL) {
				self->in_plugin_action = FALSE;
				for (i = 0; i < gs_app_list_length (state->apps); i++) {
					GsApp *app = gs_app_list_index (state->apps, i);
					gs_app_set_state (app, GS_APP_STATE_UPDATABLE);
				}
				g_task_return_error (state->task, g_steal_pointer (&local_error));
				update_state_free (state);
				return;
			}

			g_subprocess_communicate_async (subprocess, NULL, state->cancellable, update_command_communicate_cb, state);
			return;
		}
		state->step++;
	}

	if (state->step == 1) {
		/* Step 1: User declarative apps update */
		gboolean run_user = (self->user_backend == GS_NIXOS_BACKEND_DECLARATIVE) &&
		                    (gs_app_list_length (state->apps) == 0 || gs_app_list_length (state->user_apps) > 0);

		if (run_user) {
			g_autofree gchar *flake_nix = g_build_filename (self->home_manager_flake_dir, "flake.nix", NULL);
			gboolean use_flake = g_file_test (flake_nix, G_FILE_TEST_EXISTS);
			g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func (g_free);
			g_autoptr(GError) local_error = NULL;
			g_autoptr(GSubprocess) subprocess = NULL;

			g_ptr_array_add (argv, g_strdup ("sh"));
			g_ptr_array_add (argv, g_strdup ("-c"));
			if (use_flake) {
				g_ptr_array_add (argv, g_strdup_printf ("nix flake update --flake %s && home-manager switch",
				                                        self->home_manager_flake_dir));
			} else {
				g_ptr_array_add (argv, g_strdup ("nix-channel --update && home-manager switch"));
			}
			g_ptr_array_add (argv, NULL);

			subprocess = g_subprocess_newv ((const gchar * const *) argv->pdata,
			                                G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
			                                &local_error);
			if (subprocess == NULL) {
				self->in_plugin_action = FALSE;
				for (i = 0; i < gs_app_list_length (state->apps); i++) {
					GsApp *app = gs_app_list_index (state->apps, i);
					gs_app_set_state (app, GS_APP_STATE_UPDATABLE);
				}
				g_task_return_error (state->task, g_steal_pointer (&local_error));
				update_state_free (state);
				return;
			}

			g_subprocess_communicate_async (subprocess, NULL, state->cancellable, update_command_communicate_cb, state);
			return;
		}
		state->step++;
	}

	if (state->step == 2) {
		/* Step 2: Nix profile apps update */
		gboolean run_profile = (self->user_backend == GS_NIXOS_BACKEND_PROFILE) &&
		                       (gs_app_list_length (state->apps) == 0 || gs_app_list_length (state->profile_apps) > 0);

		if (run_profile) {
			g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func (g_free);
			g_autoptr(GError) local_error = NULL;
			g_autoptr(GSubprocess) subprocess = NULL;

			g_ptr_array_add (argv, g_strdup ("nix"));
			g_ptr_array_add (argv, g_strdup ("profile"));
			g_ptr_array_add (argv, g_strdup ("upgrade"));
			if (gs_app_list_length (state->profile_apps) > 0) {
				for (i = 0; i < gs_app_list_length (state->profile_apps); i++) {
					GsApp *app = gs_app_list_index (state->profile_apps, i);
					const gchar *name = gs_app_get_id (app);
					g_ptr_array_add (argv, g_strdup (name));
				}
			} else {
				g_ptr_array_add (argv, g_strdup ("--all"));
			}
			g_ptr_array_add (argv, NULL);

			subprocess = g_subprocess_newv ((const gchar * const *) argv->pdata,
			                                G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
			                                &local_error);
			if (subprocess == NULL) {
				self->in_plugin_action = FALSE;
				for (i = 0; i < gs_app_list_length (state->apps); i++) {
					GsApp *app = gs_app_list_index (state->apps, i);
					gs_app_set_state (app, GS_APP_STATE_UPDATABLE);
				}
				g_task_return_error (state->task, g_steal_pointer (&local_error));
				update_state_free (state);
				return;
			}

			g_subprocess_communicate_async (subprocess, NULL, state->cancellable, update_command_communicate_cb, state);
			return;
		}
		state->step++;
	}

	if (state->step == 3) {
		/* Step 3: Nix env apps update */
		gboolean run_env = (self->user_backend == GS_NIXOS_BACKEND_ENV) &&
		                   (gs_app_list_length (state->apps) == 0 || gs_app_list_length (state->env_apps) > 0);

		if (run_env) {
			g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func (g_free);
			g_autoptr(GError) local_error = NULL;
			g_autoptr(GSubprocess) subprocess = NULL;

			g_ptr_array_add (argv, g_strdup ("nix-env"));
			g_ptr_array_add (argv, g_strdup ("-u"));
			if (gs_app_list_length (state->env_apps) > 0) {
				for (i = 0; i < gs_app_list_length (state->env_apps); i++) {
					GsApp *app = gs_app_list_index (state->env_apps, i);
					const gchar *name = gs_app_get_id (app);
					g_ptr_array_add (argv, g_strdup (name));
				}
			}
			g_ptr_array_add (argv, NULL);

			subprocess = g_subprocess_newv ((const gchar * const *) argv->pdata,
			                                G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
			                                &local_error);
			if (subprocess == NULL) {
				self->in_plugin_action = FALSE;
				for (i = 0; i < gs_app_list_length (state->apps); i++) {
					GsApp *app = gs_app_list_index (state->apps, i);
					gs_app_set_state (app, GS_APP_STATE_UPDATABLE);
				}
				g_task_return_error (state->task, g_steal_pointer (&local_error));
				update_state_free (state);
				return;
			}

			g_subprocess_communicate_async (subprocess, NULL, state->cancellable, update_command_communicate_cb, state);
			return;
		}
		state->step++;
	}

	/* All steps completed successfully */
	self->in_plugin_action = FALSE;
	for (i = 0; i < gs_app_list_length (state->apps); i++) {
		GsApp *app = gs_app_list_index (state->apps, i);
		gs_app_set_state (app, GS_APP_STATE_INSTALLED);
	}
	g_task_return_boolean (state->task, TRUE);
	update_state_free (state);
}

static void
gs_plugin_nixos_update_apps_async (GsPlugin                           *plugin,
                                   GsAppList                          *apps,
                                   GsPluginUpdateAppsFlags             flags,
                                   GsPluginProgressCallback            progress_callback,
                                   gpointer                            progress_user_data,
                                   GsPluginEventCallback               event_callback,
                                   void                               *event_user_data,
                                   GsPluginAppNeedsUserActionCallback  app_needs_user_action_callback,
                                   gpointer                            app_needs_user_action_data,
                                   GCancellable                       *cancellable,
                                   GAsyncReadyCallback                 callback,
                                   gpointer                            user_data)
{
	GsPluginNixos *self = GS_PLUGIN_NIXOS (plugin);
	g_autoptr(GTask) task = g_task_new (plugin, cancellable, callback, user_data);
	UpdateState *state = NULL;
	gboolean has_system;
	gboolean has_user;
	gboolean has_profile;
	gboolean has_env;
	guint i;

	g_task_set_source_tag (task, gs_plugin_nixos_update_apps_async);

	self->in_plugin_action = TRUE;
	load_config (self);

	for (i = 0; i < gs_app_list_length (apps); i++) {
		GsApp *app = gs_app_list_index (apps, i);
		gs_app_set_state (app, GS_APP_STATE_INSTALLING);
	}

	has_system = (self->system_backend == GS_NIXOS_BACKEND_DECLARATIVE);
	has_user   = (self->user_backend   == GS_NIXOS_BACKEND_DECLARATIVE);
	has_profile = (self->user_backend  == GS_NIXOS_BACKEND_PROFILE);
	has_env    = (self->user_backend   == GS_NIXOS_BACKEND_ENV);

	state = g_slice_new0 (UpdateState);
	state->self = self;
	state->task = g_object_ref (task);
	state->cancellable = (cancellable != NULL) ? g_object_ref (cancellable) : NULL;
	state->apps = g_object_ref (apps);
	state->system_apps = gs_app_list_new ();
	state->user_apps = gs_app_list_new ();
	state->profile_apps = gs_app_list_new ();
	state->env_apps = gs_app_list_new ();
	state->step = 0;

	for (i = 0; i < gs_app_list_length (apps); i++) {
		GsApp *app = gs_app_list_index (apps, i);
		g_autofree gchar *pkg_name = guess_nix_package_name (app);
		const gchar *src = gs_app_get_metadata_item (app, "NixOS::install-source");
		if (src == NULL) {
			src = detect_package_install_source (self, pkg_name);
		}

		if (g_strcmp0 (src, "system") == 0 && has_system)
			gs_app_list_add (state->system_apps, app);
		else if (g_strcmp0 (src, "user") == 0 && has_user)
			gs_app_list_add (state->user_apps, app);
		else if (g_strcmp0 (src, "env") == 0 || has_env)
			gs_app_list_add (state->env_apps, app);
		else if (has_profile)
			gs_app_list_add (state->profile_apps, app);
		else if (has_system)
			gs_app_list_add (state->system_apps, app);
		else
			gs_app_list_add (state->profile_apps, app);
	}

	update_next_step (state);
}

static gboolean
gs_plugin_nixos_update_apps_finish (GsPlugin *plugin, GAsyncResult *result, GError **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
}

static void
gs_plugin_nixos_refine_async (GsPlugin                   *plugin,
                               GsAppList                  *list,
                               GsPluginRefineFlags         job_flags,
                               GsPluginRefineRequireFlags  require_flags,
                               GsPluginEventCallback       event_callback,
                               void                       *event_user_data,
                               GCancellable               *cancellable,
                               GAsyncReadyCallback         callback,
                               gpointer                    user_data)
{
	GsPluginNixos *self = GS_PLUGIN_NIXOS (plugin);
	g_autoptr(GTask) task = g_task_new (plugin, cancellable, callback, user_data);
	guint i;

	g_task_set_source_tag (task, gs_plugin_nixos_refine_async);

	load_config (self);

	for (i = 0; i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);
		g_autofree gchar *pkg_name = NULL;
		const gchar *install_src = NULL;

		if (!gs_app_has_management_plugin (app, plugin))
			continue;

		pkg_name = guess_nix_package_name (app);
		if (pkg_name == NULL)
			continue;

		/* Refine state */
		install_src = detect_package_install_source (self, pkg_name);
		if (install_src != NULL) {
			if (gs_app_get_state (app) != GS_APP_STATE_INSTALLED)
				gs_app_set_state (app, GS_APP_STATE_INSTALLED);
			gs_app_set_metadata (app, "NixOS::install-source", install_src);
		} else {
			if (gs_app_get_state (app) != GS_APP_STATE_AVAILABLE)
				gs_app_set_state (app, GS_APP_STATE_AVAILABLE);
		}

		/* Refine packaging metadata */
		if (gs_app_get_origin (app) == NULL)
			gs_app_set_origin (app, "nixpkgs");
		gs_app_set_origin_ui (app, "NixOS");
		gs_app_set_metadata (app, "GnomeSoftware::PackagingFormat", "Nix");
		gs_app_set_metadata (app, "GnomeSoftware::PackagingBaseCssColor", "accent_color");
	}

	g_task_return_boolean (task, TRUE);
}

static gboolean
gs_plugin_nixos_refine_finish (GsPlugin      *plugin,
                               GAsyncResult  *result,
                               GError       **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
}

static void
gs_plugin_nixos_class_init (GsPluginNixosClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GsPluginClass *plugin_class = GS_PLUGIN_CLASS (klass);

	object_class->dispose = gs_plugin_nixos_dispose;

	plugin_class->adopt_app = gs_plugin_nixos_adopt_app;
	plugin_class->setup_async = gs_plugin_nixos_setup_async;
	plugin_class->setup_finish = gs_plugin_nixos_setup_finish;
	plugin_class->list_apps_async = gs_plugin_nixos_list_apps_async;
	plugin_class->list_apps_finish = gs_plugin_nixos_list_apps_finish;
	plugin_class->install_apps_async = gs_plugin_nixos_install_apps_async;
	plugin_class->install_apps_finish = gs_plugin_nixos_install_apps_finish;
	plugin_class->uninstall_apps_async = gs_plugin_nixos_uninstall_apps_async;
	plugin_class->uninstall_apps_finish = gs_plugin_nixos_uninstall_apps_finish;
	plugin_class->update_apps_async = gs_plugin_nixos_update_apps_async;
	plugin_class->update_apps_finish = gs_plugin_nixos_update_apps_finish;
	plugin_class->refine_async = gs_plugin_nixos_refine_async;
	plugin_class->refine_finish = gs_plugin_nixos_refine_finish;
}

GType
gs_plugin_query_type (void)
{
	return GS_TYPE_PLUGIN_NIXOS;
}
