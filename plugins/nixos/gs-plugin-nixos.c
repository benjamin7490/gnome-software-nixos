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

typedef enum {
	GS_PLUGIN_NIXOS_MODE_NIX_PROFILE,
	GS_PLUGIN_NIXOS_MODE_NIX_ENV,
	GS_PLUGIN_NIXOS_MODE_DECLARATIVE_SYSTEM,
	GS_PLUGIN_NIXOS_MODE_DECLARATIVE_USER
} GsPluginNixosMode;

struct _GsPluginNixos
{
	GsPlugin		parent;
	GsPluginNixosMode	mode;
	gchar			*flake_uri;
	gchar			*declarative_system_file;
	gchar			*declarative_user_file;
	gchar			*system_flake_dir;
	gchar			*home_manager_flake_dir;
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
	self->flake_uri = NULL;
	self->declarative_system_file = NULL;
	self->declarative_user_file = NULL;
	self->system_flake_dir = NULL;
	self->home_manager_flake_dir = NULL;
}

static void
gs_plugin_nixos_dispose (GObject *object)
{
	GsPluginNixos *self = GS_PLUGIN_NIXOS (object);

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

static void
load_config (GsPluginNixos *self)
{
	g_autofree gchar *config_dir = g_build_filename (g_get_user_config_dir (), "gnome-software", NULL);
	g_autofree gchar *config_file = g_build_filename (config_dir, "nixos.json", NULL);

	/* Set defaults */
	self->mode = GS_PLUGIN_NIXOS_MODE_NIX_PROFILE;
	self->flake_uri = g_strdup ("nixpkgs");
	self->declarative_system_file = g_strdup ("/etc/nixos/gnome-software-packages.nix");
	self->declarative_user_file = g_build_filename (g_get_home_dir (), ".config", "home-manager", "gnome-software-packages.nix", NULL);
	self->system_flake_dir = g_strdup ("/etc/nixos");
	self->home_manager_flake_dir = g_build_filename (g_get_home_dir (), ".config", "home-manager", NULL);

	/* Auto-detect fallback: if /etc/nixos/flake.nix doesn't exist, check if legacy nix-env mode is better */
	g_autofree gchar *flake_nix = g_build_filename ("/etc/nixos", "flake.nix", NULL);
	if (!g_file_test (flake_nix, G_FILE_TEST_EXISTS)) {
		self->mode = GS_PLUGIN_NIXOS_MODE_NIX_ENV;
	}

	if (g_file_test (config_file, G_FILE_TEST_EXISTS)) {
		g_autoptr(JsonParser) parser = json_parser_new ();
		g_autoptr(GError) error = NULL;
		if (json_parser_load_from_file (parser, config_file, &error)) {
			JsonNode *root = json_parser_get_root (parser);
			if (JSON_NODE_HOLDS_OBJECT (root)) {
				JsonObject *obj = json_node_get_object (root);
				if (json_object_has_member (obj, "mode")) {
					const gchar *mode_str = json_object_get_string_member (obj, "mode");
					if (g_strcmp0 (mode_str, "nix-profile") == 0)
						self->mode = GS_PLUGIN_NIXOS_MODE_NIX_PROFILE;
					else if (g_strcmp0 (mode_str, "nix-env") == 0)
						self->mode = GS_PLUGIN_NIXOS_MODE_NIX_ENV;
					else if (g_strcmp0 (mode_str, "declarative-system") == 0)
						self->mode = GS_PLUGIN_NIXOS_MODE_DECLARATIVE_SYSTEM;
					else if (g_strcmp0 (mode_str, "declarative-user") == 0)
						self->mode = GS_PLUGIN_NIXOS_MODE_DECLARATIVE_USER;
				}
				if (json_object_has_member (obj, "flake_uri")) {
					g_free (self->flake_uri);
					self->flake_uri = g_strdup (json_object_get_string_member (obj, "flake_uri"));
				}
				if (json_object_has_member (obj, "declarative_system_file")) {
					g_free (self->declarative_system_file);
					self->declarative_system_file = expand_path (json_object_get_string_member (obj, "declarative_system_file"));
				}
				if (json_object_has_member (obj, "declarative_user_file")) {
					g_free (self->declarative_user_file);
					self->declarative_user_file = expand_path (json_object_get_string_member (obj, "declarative_user_file"));
				}
				if (json_object_has_member (obj, "system_flake_dir")) {
					g_free (self->system_flake_dir);
					self->system_flake_dir = expand_path (json_object_get_string_member (obj, "system_flake_dir"));
				}
				if (json_object_has_member (obj, "home_manager_flake_dir")) {
					g_free (self->home_manager_flake_dir);
					self->home_manager_flake_dir = expand_path (json_object_get_string_member (obj, "home_manager_flake_dir"));
				}
			}
		} else {
			g_warning ("Failed to load NixOS GS plugin config: %s", error->message);
		}
	} else {
		/* Save default config */
		g_mkdir_with_parents (config_dir, 0755);
		g_autoptr(JsonBuilder) builder = json_builder_new ();
		json_builder_begin_object (builder);
		const gchar *mode_str = "nix-profile";
		if (self->mode == GS_PLUGIN_NIXOS_MODE_NIX_ENV) mode_str = "nix-env";
		json_builder_set_member_name (builder, "mode");
		json_builder_add_string_value (builder, mode_str);
		json_builder_set_member_name (builder, "flake_uri");
		json_builder_add_string_value (builder, self->flake_uri);
		json_builder_set_member_name (builder, "declarative_system_file");
		json_builder_add_string_value (builder, self->declarative_system_file);
		json_builder_set_member_name (builder, "declarative_user_file");
		json_builder_add_string_value (builder, self->declarative_user_file);
		json_builder_set_member_name (builder, "system_flake_dir");
		json_builder_add_string_value (builder, self->system_flake_dir);
		json_builder_set_member_name (builder, "home_manager_flake_dir");
		json_builder_add_string_value (builder, self->home_manager_flake_dir);
		json_builder_end_object (builder);

		g_autoptr(JsonGenerator) gen = json_generator_new ();
		g_autoptr(JsonNode) node = json_builder_get_root (builder);
		json_generator_set_root (gen, node);
		g_autoptr(GError) error = NULL;
		if (!json_generator_to_file (gen, config_file, &error)) {
			g_warning ("Failed to write NixOS GS plugin default config: %s", error->message);
		}
	}
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

	for (guint i = 0; i < packages->len; i++) {
		const gchar *pkg = g_ptr_array_index (packages, i);
		for (guint j = 0; j < G_N_ELEMENTS (special_options); j++) {
			if (g_strcmp0 (pkg, special_options[j].package_name) == 0) {
				g_string_append_printf (content, "  %s\n", special_options[j].option_nix);
				break;
			}
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
		g_autoptr(GsApp) app = gs_app_new (name);
		gs_app_set_state (app, GS_APP_STATE_INSTALLED);
		gs_app_set_kind (app, AS_COMPONENT_KIND_GENERIC);
		gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_PACKAGE);
		gs_app_set_management_plugin (app, plugin);
		gs_app_add_source (app, name);
		gs_app_list_add (list, app);
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
		if (version != NULL) {
			gs_app_set_version (app, version);
		}

		gs_app_list_add (list, app);
	}
	g_list_free (members);
	return g_steal_pointer (&list);
}

static void
gs_plugin_nixos_adopt_app (GsPlugin *plugin, GsApp *app)
{
	if (gs_app_get_bundle_kind (app) == AS_BUNDLE_KIND_PACKAGE) {
		gs_app_set_management_plugin (app, plugin);
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

	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "appstream");
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_BEFORE, "packagekit");

	g_task_return_boolean (task, TRUE);
}

static gboolean
gs_plugin_nixos_setup_finish (GsPlugin *plugin, GAsyncResult *result, GError **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
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
	g_task_set_source_tag (task, gs_plugin_nixos_list_apps_async);

	if (query == NULL) {
		g_task_return_pointer (task, gs_app_list_new (), g_object_unref);
		return;
	}

	GsAppQueryTristate is_installed = gs_app_query_get_is_installed (query);
	if (is_installed != GS_APP_QUERY_TRISTATE_TRUE) {
		g_task_return_pointer (task, gs_app_list_new (), g_object_unref);
		return;
	}

	if (self->mode == GS_PLUGIN_NIXOS_MODE_DECLARATIVE_SYSTEM) {
		g_autoptr(GError) local_error = NULL;
		g_autoptr(GsAppList) list = list_declarative_packages (plugin, self->declarative_system_file, &local_error);
		if (list == NULL) {
			g_task_return_error (task, g_steal_pointer (&local_error));
		} else {
			g_task_return_pointer (task, g_steal_pointer (&list), g_object_unref);
		}
		return;
	}

	if (self->mode == GS_PLUGIN_NIXOS_MODE_DECLARATIVE_USER) {
		g_autoptr(GError) local_error = NULL;
		g_autoptr(GsAppList) list = list_declarative_packages (plugin, self->declarative_user_file, &local_error);
		if (list == NULL) {
			g_task_return_error (task, g_steal_pointer (&local_error));
		} else {
			g_task_return_pointer (task, g_steal_pointer (&list), g_object_unref);
		}
		return;
	}

	g_autoptr(GSubprocess) subprocess = NULL;
	g_autoptr(GError) local_error = NULL;
	if (self->mode == GS_PLUGIN_NIXOS_MODE_NIX_PROFILE) {
		subprocess = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
		                               &local_error,
		                               "nix", "profile", "list", "--json", NULL);
	} else {
		subprocess = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
		                               &local_error,
		                               "nix-env", "-q", "--json", NULL);
	}

	if (subprocess == NULL) {
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	ListAppsData *data = g_slice_new0 (ListAppsData);
	data->subprocess = g_object_ref (subprocess);
	data->self = self;

	g_task_set_task_data (task, data, (GDestroyNotify) list_apps_data_free);
	g_subprocess_communicate_async (subprocess, NULL, cancellable, list_apps_communicate_cb, g_steal_pointer (&task));
}

static void
list_apps_communicate_cb (GObject *source_object, GAsyncResult *result, gpointer user_data)
{
	GSubprocess *subprocess = G_SUBPROCESS (source_object);
	g_autoptr(GTask) task = G_TASK (user_data);
	ListAppsData *data = g_task_get_task_data (task);
	g_autoptr(GError) local_error = NULL;
	g_autoptr(GBytes) stdout_buf = NULL;

	if (!g_subprocess_communicate_finish (subprocess, result, &stdout_buf, NULL, &local_error)) {
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	const gchar *output = g_bytes_get_data (stdout_buf, NULL);
	if (output == NULL) output = "";

	g_autoptr(GsAppList) list = NULL;
	if (data->self->mode == GS_PLUGIN_NIXOS_MODE_NIX_PROFILE) {
		list = parse_nix_profile_json (GS_PLUGIN (data->self), output, &local_error);
	} else {
		list = parse_nix_env_json (GS_PLUGIN (data->self), output, &local_error);
	}

	if (list == NULL) {
		g_task_return_error (task, g_steal_pointer (&local_error));
	} else {
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
	g_autoptr(GError) local_error = NULL;
	g_autoptr(GBytes) stderr_buf = NULL;

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

	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func (g_free);
	gboolean use_pkexec = FALSE;

	for (guint i = 0; i < gs_app_list_length (apps); i++) {
		GsApp *app = gs_app_list_index (apps, i);
		gs_app_set_state (app, GS_APP_STATE_INSTALLING);
	}

	if (self->mode == GS_PLUGIN_NIXOS_MODE_NIX_PROFILE) {
		g_ptr_array_add (argv, g_strdup ("nix"));
		g_ptr_array_add (argv, g_strdup ("profile"));
		g_ptr_array_add (argv, g_strdup ("add"));
		for (guint i = 0; i < gs_app_list_length (apps); i++) {
			GsApp *app = gs_app_list_index (apps, i);
			const gchar *name = gs_app_get_id (app);
			g_ptr_array_add (argv, g_strdup_printf ("%s#%s", self->flake_uri, name));
		}
		g_ptr_array_add (argv, NULL);
	} else if (self->mode == GS_PLUGIN_NIXOS_MODE_NIX_ENV) {
		g_ptr_array_add (argv, g_strdup ("nix-env"));
		g_ptr_array_add (argv, g_strdup ("-iA"));
		for (guint i = 0; i < gs_app_list_length (apps); i++) {
			GsApp *app = gs_app_list_index (apps, i);
			const gchar *name = gs_app_get_id (app);
			g_ptr_array_add (argv, g_strdup_printf ("nixpkgs.%s", name));
		}
		g_ptr_array_add (argv, NULL);
	} else if (self->mode == GS_PLUGIN_NIXOS_MODE_DECLARATIVE_SYSTEM) {
		g_autoptr(GError) local_error = NULL;
		g_autoptr(GPtrArray) packages = parse_nix_file_packages (self->declarative_system_file);
		for (guint i = 0; i < gs_app_list_length (apps); i++) {
			GsApp *app = gs_app_list_index (apps, i);
			const gchar *name = gs_app_get_id (app);
			gboolean found = FALSE;
			for (guint j = 0; j < packages->len; j++) {
				if (g_strcmp0 (g_ptr_array_index (packages, j), name) == 0) {
					found = TRUE;
					break;
				}
			}
			if (!found) {
				g_ptr_array_add (packages, g_strdup (name));
			}
		}

		const gchar *tmp_file = "/tmp/gnome-software-packages.nix.tmp";
		if (!write_nix_file_packages (tmp_file, packages, FALSE, &local_error)) {
			g_task_return_error (task, g_steal_pointer (&local_error));
			return;
		}

		g_autofree gchar *flake_nix = g_build_filename (self->system_flake_dir, "flake.nix", NULL);
		gboolean use_flake = g_file_test (flake_nix, G_FILE_TEST_EXISTS);

		use_pkexec = TRUE;
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
	} else if (self->mode == GS_PLUGIN_NIXOS_MODE_DECLARATIVE_USER) {
		g_autoptr(GError) local_error = NULL;
		g_autoptr(GPtrArray) packages = parse_nix_file_packages (self->declarative_user_file);
		for (guint i = 0; i < gs_app_list_length (apps); i++) {
			GsApp *app = gs_app_list_index (apps, i);
			const gchar *name = gs_app_get_id (app);
			gboolean found = FALSE;
			for (guint j = 0; j < packages->len; j++) {
				if (g_strcmp0 (g_ptr_array_index (packages, j), name) == 0) {
					found = TRUE;
					break;
				}
			}
			if (!found) {
				g_ptr_array_add (packages, g_strdup (name));
			}
		}

		if (!write_nix_file_packages (self->declarative_user_file, packages, TRUE, &local_error)) {
			g_task_return_error (task, g_steal_pointer (&local_error));
			return;
		}

		g_ptr_array_add (argv, g_strdup ("home-manager"));
		g_ptr_array_add (argv, g_strdup ("switch"));
		g_ptr_array_add (argv, NULL);
	}

	g_autoptr(GSubprocess) subprocess = NULL;
	g_autoptr(GError) local_error = NULL;
	gchar **cmd_argv = (gchar **) argv->pdata;
	g_auto(GStrv) pkexec_argv = NULL;

	if (use_pkexec) {
		guint len = argv->len - 1;
		pkexec_argv = g_new0 (gchar *, len + 2);
		pkexec_argv[0] = g_strdup ("pkexec");
		for (guint i = 0; i < len; i++) {
			pkexec_argv[i+1] = g_strdup (cmd_argv[i]);
		}
		pkexec_argv[len+1] = NULL;
		cmd_argv = pkexec_argv;
	}

	subprocess = g_subprocess_newv ((const gchar * const *) cmd_argv,
	                                G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
	                                &local_error);

	if (subprocess == NULL) {
		for (guint i = 0; i < gs_app_list_length (apps); i++) {
			GsApp *app = gs_app_list_index (apps, i);
			gs_app_set_state (app, GS_APP_STATE_AVAILABLE);
		}
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	g_task_set_task_data (task, g_object_ref (subprocess), g_object_unref);
	g_subprocess_communicate_async (subprocess, NULL, cancellable, command_communicate_cb, g_steal_pointer (&task));
}

static gboolean
gs_plugin_nixos_install_apps_finish (GsPlugin *plugin, GAsyncResult *result, GError **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
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
	g_task_set_source_tag (task, gs_plugin_nixos_uninstall_apps_async);

	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func (g_free);
	gboolean use_pkexec = FALSE;

	for (guint i = 0; i < gs_app_list_length (apps); i++) {
		GsApp *app = gs_app_list_index (apps, i);
		gs_app_set_state (app, GS_APP_STATE_REMOVING);
	}

	if (self->mode == GS_PLUGIN_NIXOS_MODE_NIX_PROFILE) {
		g_ptr_array_add (argv, g_strdup ("nix"));
		g_ptr_array_add (argv, g_strdup ("profile"));
		g_ptr_array_add (argv, g_strdup ("remove"));
		for (guint i = 0; i < gs_app_list_length (apps); i++) {
			GsApp *app = gs_app_list_index (apps, i);
			const gchar *name = gs_app_get_id (app);
			g_ptr_array_add (argv, g_strdup (name));
		}
		g_ptr_array_add (argv, NULL);
	} else if (self->mode == GS_PLUGIN_NIXOS_MODE_NIX_ENV) {
		g_ptr_array_add (argv, g_strdup ("nix-env"));
		g_ptr_array_add (argv, g_strdup ("-e"));
		for (guint i = 0; i < gs_app_list_length (apps); i++) {
			GsApp *app = gs_app_list_index (apps, i);
			const gchar *name = gs_app_get_id (app);
			g_ptr_array_add (argv, g_strdup (name));
		}
		g_ptr_array_add (argv, NULL);
	} else if (self->mode == GS_PLUGIN_NIXOS_MODE_DECLARATIVE_SYSTEM) {
		g_autoptr(GError) local_error = NULL;
		g_autoptr(GPtrArray) packages = parse_nix_file_packages (self->declarative_system_file);
		for (guint i = 0; i < gs_app_list_length (apps); i++) {
			GsApp *app = gs_app_list_index (apps, i);
			const gchar *name = gs_app_get_id (app);
			for (guint j = 0; j < packages->len; j++) {
				if (g_strcmp0 (g_ptr_array_index (packages, j), name) == 0) {
					g_ptr_array_remove_index (packages, j);
					break;
				}
			}
		}

		const gchar *tmp_file = "/tmp/gnome-software-packages.nix.tmp";
		if (!write_nix_file_packages (tmp_file, packages, FALSE, &local_error)) {
			g_task_return_error (task, g_steal_pointer (&local_error));
			return;
		}

		g_autofree gchar *flake_nix = g_build_filename (self->system_flake_dir, "flake.nix", NULL);
		gboolean use_flake = g_file_test (flake_nix, G_FILE_TEST_EXISTS);

		use_pkexec = TRUE;
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
	} else if (self->mode == GS_PLUGIN_NIXOS_MODE_DECLARATIVE_USER) {
		g_autoptr(GError) local_error = NULL;
		g_autoptr(GPtrArray) packages = parse_nix_file_packages (self->declarative_user_file);
		for (guint i = 0; i < gs_app_list_length (apps); i++) {
			GsApp *app = gs_app_list_index (apps, i);
			const gchar *name = gs_app_get_id (app);
			for (guint j = 0; j < packages->len; j++) {
				if (g_strcmp0 (g_ptr_array_index (packages, j), name) == 0) {
					g_ptr_array_remove_index (packages, j);
					break;
				}
			}
		}

		if (!write_nix_file_packages (self->declarative_user_file, packages, TRUE, &local_error)) {
			g_task_return_error (task, g_steal_pointer (&local_error));
			return;
		}

		g_ptr_array_add (argv, g_strdup ("home-manager"));
		g_ptr_array_add (argv, g_strdup ("switch"));
		g_ptr_array_add (argv, NULL);
	}

	g_autoptr(GSubprocess) subprocess = NULL;
	g_autoptr(GError) local_error = NULL;
	gchar **cmd_argv = (gchar **) argv->pdata;
	g_auto(GStrv) pkexec_argv = NULL;

	if (use_pkexec) {
		guint len = argv->len - 1;
		pkexec_argv = g_new0 (gchar *, len + 2);
		pkexec_argv[0] = g_strdup ("pkexec");
		for (guint i = 0; i < len; i++) {
			pkexec_argv[i+1] = g_strdup (cmd_argv[i]);
		}
		pkexec_argv[len+1] = NULL;
		cmd_argv = pkexec_argv;
	}

	subprocess = g_subprocess_newv ((const gchar * const *) cmd_argv,
	                                G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
	                                &local_error);

	if (subprocess == NULL) {
		for (guint i = 0; i < gs_app_list_length (apps); i++) {
			GsApp *app = gs_app_list_index (apps, i);
			gs_app_set_state (app, GS_APP_STATE_INSTALLED);
		}
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	g_task_set_task_data (task, g_object_ref (subprocess), g_object_unref);
	g_subprocess_communicate_async (subprocess, NULL, cancellable, command_communicate_cb, g_steal_pointer (&task));
}

static gboolean
gs_plugin_nixos_uninstall_apps_finish (GsPlugin *plugin, GAsyncResult *result, GError **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
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
	g_task_set_source_tag (task, gs_plugin_nixos_update_apps_async);

	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func (g_free);
	gboolean use_pkexec = FALSE;

	for (guint i = 0; i < gs_app_list_length (apps); i++) {
		GsApp *app = gs_app_list_index (apps, i);
		gs_app_set_state (app, GS_APP_STATE_INSTALLING);
	}

	if (self->mode == GS_PLUGIN_NIXOS_MODE_NIX_PROFILE) {
		g_ptr_array_add (argv, g_strdup ("nix"));
		g_ptr_array_add (argv, g_strdup ("profile"));
		g_ptr_array_add (argv, g_strdup ("upgrade"));
		if (gs_app_list_length (apps) > 0) {
			for (guint i = 0; i < gs_app_list_length (apps); i++) {
				GsApp *app = gs_app_list_index (apps, i);
				const gchar *name = gs_app_get_id (app);
				g_ptr_array_add (argv, g_strdup (name));
			}
		} else {
			g_ptr_array_add (argv, g_strdup ("--all"));
		}
		g_ptr_array_add (argv, NULL);
	} else if (self->mode == GS_PLUGIN_NIXOS_MODE_NIX_ENV) {
		g_ptr_array_add (argv, g_strdup ("nix-env"));
		g_ptr_array_add (argv, g_strdup ("-u"));
		if (gs_app_list_length (apps) > 0) {
			for (guint i = 0; i < gs_app_list_length (apps); i++) {
				GsApp *app = gs_app_list_index (apps, i);
				const gchar *name = gs_app_get_id (app);
				g_ptr_array_add (argv, g_strdup (name));
			}
		}
		g_ptr_array_add (argv, NULL);
	} else if (self->mode == GS_PLUGIN_NIXOS_MODE_DECLARATIVE_SYSTEM) {
		g_autofree gchar *flake_nix = g_build_filename (self->system_flake_dir, "flake.nix", NULL);
		gboolean use_flake = g_file_test (flake_nix, G_FILE_TEST_EXISTS);

		use_pkexec = TRUE;
		g_ptr_array_add (argv, g_strdup ("sh"));
		g_ptr_array_add (argv, g_strdup ("-c"));
		if (use_flake) {
			g_ptr_array_add (argv, g_strdup_printf ("nix flake update --flake %s && nixos-rebuild switch --flake %s",
			                                        self->system_flake_dir, self->system_flake_dir));
		} else {
			g_ptr_array_add (argv, g_strdup ("nix-channel --update nixos && nixos-rebuild switch"));
		}
		g_ptr_array_add (argv, NULL);
	} else if (self->mode == GS_PLUGIN_NIXOS_MODE_DECLARATIVE_USER) {
		g_autofree gchar *flake_nix = g_build_filename (self->home_manager_flake_dir, "flake.nix", NULL);
		gboolean use_flake = g_file_test (flake_nix, G_FILE_TEST_EXISTS);

		g_ptr_array_add (argv, g_strdup ("sh"));
		g_ptr_array_add (argv, g_strdup ("-c"));
		if (use_flake) {
			g_ptr_array_add (argv, g_strdup_printf ("nix flake update --flake %s && home-manager switch",
			                                        self->home_manager_flake_dir));
		} else {
			g_ptr_array_add (argv, g_strdup ("nix-channel --update && home-manager switch"));
		}
		g_ptr_array_add (argv, NULL);
	}

	g_autoptr(GSubprocess) subprocess = NULL;
	g_autoptr(GError) local_error = NULL;
	gchar **cmd_argv = (gchar **) argv->pdata;
	g_auto(GStrv) pkexec_argv = NULL;

	if (use_pkexec) {
		guint len = argv->len - 1;
		pkexec_argv = g_new0 (gchar *, len + 2);
		pkexec_argv[0] = g_strdup ("pkexec");
		for (guint i = 0; i < len; i++) {
			pkexec_argv[i+1] = g_strdup (cmd_argv[i]);
		}
		pkexec_argv[len+1] = NULL;
		cmd_argv = pkexec_argv;
	}

	subprocess = g_subprocess_newv ((const gchar * const *) cmd_argv,
	                                G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
	                                &local_error);

	if (subprocess == NULL) {
		for (guint i = 0; i < gs_app_list_length (apps); i++) {
			GsApp *app = gs_app_list_index (apps, i);
			gs_app_set_state (app, GS_APP_STATE_UPDATABLE);
		}
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	g_task_set_task_data (task, g_object_ref (subprocess), g_object_unref);
	g_subprocess_communicate_async (subprocess, NULL, cancellable, command_communicate_cb, g_steal_pointer (&task));
}

static gboolean
gs_plugin_nixos_update_apps_finish (GsPlugin *plugin, GAsyncResult *result, GError **error)
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
}

GType
gs_plugin_query_type (void)
{
	return GS_TYPE_PLUGIN_NIXOS;
}
