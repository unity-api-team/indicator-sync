/*
   The way for apps to interact with the Sync Indicator

   Copyright 2012 Canonical Ltd.

   Authors:
     Charles Kerr <charles.kerr@canonical.com>

   This program is free software: you can redistribute it and/or modify it 
   under the terms of the GNU General Public License version 3,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranties of
   MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR PURPOSE.
   See the GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along 
   with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
 #include "config.h"
#endif

#include <string.h> /* strlen() */

#include <glib.h>

#include <libdbusmenu-glib/dbusmenu-glib.h>

#include "dbus-shared.h"
#include "sync-app.h"
#include "sync-app-dbus.h"
#include "sync-enum.h"

struct _SyncMenuAppPriv
{
  guint             watch_id;
  GDBusConnection * session_bus;
  DbusSyncMenuApp * skeleton;
  DbusmenuServer  * menu_server;
  GBinding        * menu_binding;
  gchar           * desktop_id;
  SyncMenuState     state;
  gboolean          paused;
  GCancellable    * bus_get_cancellable;
};

enum
{
  PROP_0,
  PROP_STATE,
  PROP_PAUSED,
  PROP_DESKTOP_ID,
  PROP_DBUSMENU,
  N_PROPERTIES
};

static GParamSpec * properties[N_PROPERTIES];

/* GObject stuff */
static void sync_menu_app_class_init (SyncMenuAppClass * klass);
static void sync_menu_app_init       (SyncMenuApp *self);
static void sync_menu_app_dispose    (GObject *object);
static void sync_menu_app_finalize   (GObject *object);
static void sync_menu_app_constructed  (GObject *object);
static void set_property (GObject*, guint prop_id, const GValue*, GParamSpec* );
static void get_property (GObject*, guint prop_id,       GValue*, GParamSpec* );

G_DEFINE_TYPE (SyncMenuApp, sync_menu_app, G_TYPE_OBJECT);

static void
sync_menu_app_class_init (SyncMenuAppClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (SyncMenuAppPriv));

  object_class->dispose = sync_menu_app_dispose;
  object_class->finalize = sync_menu_app_finalize;
  object_class->constructed = sync_menu_app_constructed;
  object_class->set_property = set_property;
  object_class->get_property = get_property;

  properties[PROP_STATE] = g_param_spec_enum (
    SYNC_MENU_APP_PROP_STATE,
    "State",
    "The SyncMenuState that represents this client's state",
    SYNC_MENU_TYPE_STATE,
    SYNC_MENU_STATE_IDLE,
    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_PAUSED] = g_param_spec_boolean (
    SYNC_MENU_APP_PROP_PAUSED,
    "Paused",
    "Whether or not this client is paused",
    FALSE,
    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_DESKTOP_ID] = g_param_spec_string (
    SYNC_MENU_APP_PROP_DESKTOP_ID,
    "Desktop Id",
    "The name of the .desktop file that belongs to the client app",
    NULL,
    G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  properties[PROP_DBUSMENU] = g_param_spec_object (
    SYNC_MENU_APP_PROP_DBUSMENU,
    "MenuItems",
    "The extra menuitems to display in the client's section in the Sync Indicator",
    DBUSMENU_TYPE_SERVER,
    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPERTIES, properties);
}

static void
sync_menu_app_dispose (GObject *object)
{
  SyncMenuApp * self = SYNC_MENU_APP(object);
  g_return_if_fail (self != NULL);
  SyncMenuAppPriv * p = self->priv;

  sync_menu_app_set_menu (self, NULL);

  if (p->skeleton != NULL)
    {
      GDBusInterfaceSkeleton * s = G_DBUS_INTERFACE_SKELETON(p->skeleton);

      if (g_dbus_interface_skeleton_get_connection(s) != NULL)
        g_dbus_interface_skeleton_unexport (s);

      g_clear_object (&p->skeleton);
    }

  if (p->bus_get_cancellable != NULL)
    {
      g_cancellable_cancel (p->bus_get_cancellable);
      g_object_steal_data (G_OBJECT(p->bus_get_cancellable), "sync-menu-app");
      p->bus_get_cancellable = NULL;
    }

  if (p->watch_id != 0 )
    {
      g_bus_unwatch_name (p->watch_id);
      p->watch_id = 0;
    }

  g_clear_object (&p->session_bus);

  G_OBJECT_CLASS (sync_menu_app_parent_class)->dispose (object);
}

static void
sync_menu_app_finalize (GObject *object)
{
  SyncMenuApp * self = SYNC_MENU_APP(object);
  g_return_if_fail (self != NULL);
  SyncMenuAppPriv * p = self->priv;

  g_clear_pointer (&p->desktop_id, g_free);

  G_OBJECT_CLASS (sync_menu_app_parent_class)->finalize (object);
}

static gchar*
build_path_from_desktop_id (const gchar * desktop_id)
{
  g_return_val_if_fail (desktop_id && *desktop_id, NULL);

  /* get the basename in case they passed a full filename
     instead of just the desktop id */
  gchar * base = g_path_get_basename (desktop_id);
  
  if (g_str_has_suffix (base, ".desktop"))
    base[strlen(base)-8] = '\0';

  /* dbus names only allow alnum + underscores */
  gchar * p;
  for (p=base; p && *p; ++p)
    if (!g_ascii_isalnum(*p))
      *p = '_';

  p = g_strdup_printf( "/com/canonical/indicator/sync/source/%s", base);
  g_free (base);

  if (g_variant_is_object_path (p))
    {
      g_debug (G_STRLOC" built path {%s} from desktop id {%s}", p, desktop_id);
    }
  else
    {
      g_warning (G_STRLOC" Not a valid object path: \"%s\"", p);
      g_clear_pointer (&p, g_free);
    }

  return p;
}

static void
on_sync_service_name_appeared (GDBusConnection  * connection,
                               const gchar      * name,
                               const gchar      * name_owner,
                               gpointer           user_data)
{
  g_debug (G_STRLOC" %s", G_STRFUNC);
  SyncMenuApp * app = SYNC_MENU_APP(user_data);
  SyncMenuAppPriv * p = app->priv;

  dbus_sync_menu_app_emit_exists (p->skeleton);
}

static void
on_sync_service_name_vanished (GDBusConnection  * connection,
                               const gchar      * name,
                               gpointer           user_data)
{
  g_debug (G_STRLOC" %s", G_STRFUNC);
}

static void
on_got_bus (GObject * o, GAsyncResult * res, gpointer gcancellable)
{
  GError * err;
  SyncMenuApp * app;
  SyncMenuAppPriv * p;
  GCancellable * cancellable;

  /* if we were cancelled, don't do anything. */
  cancellable = G_CANCELLABLE (gcancellable);
  g_return_if_fail (cancellable != NULL);
  if (g_cancellable_is_cancelled (cancellable))
    {
      g_object_unref (cancellable);
      return;
    }

  app = g_object_get_data (G_OBJECT(cancellable), "sync-menu-app");
  g_return_if_fail (IS_SYNC_MENU_APP (app));
  p = app->priv;
  p->bus_get_cancellable = NULL;
  g_clear_object (&cancellable);

  err = NULL;
  p->session_bus = g_bus_get_finish (res, &err);
  if (err != NULL)
    { 
      g_error ("unable to get bus: %s", err->message);
      g_clear_error (&err);
    }
  else
    {
      gchar * path;

      GDBusInterfaceSkeleton * skeleton = G_DBUS_INTERFACE_SKELETON(p->skeleton);
      path = build_path_from_desktop_id (p->desktop_id);

      if (path != NULL)
        {
          g_dbus_interface_skeleton_export (skeleton, p->session_bus, path, &err);

          if (err != NULL)
            { 
              g_error ("unable to export skeleton: %s", err->message);
              g_clear_error (&err);
            }
 
          p->watch_id = g_bus_watch_name_on_connection (p->session_bus,
                                                        SYNC_SERVICE_DBUS_NAME,
                                                        G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
                                                        on_sync_service_name_appeared,
                                                        on_sync_service_name_vanished,
                                                        app, NULL);

          g_free (path);
        }
    }
}

static void
sync_menu_app_init (SyncMenuApp * client)
{
  SyncMenuAppPriv * p;

  p = G_TYPE_INSTANCE_GET_PRIVATE (client, SYNC_MENU_APP_TYPE, SyncMenuAppPriv);
  p->paused = FALSE;
  p->menu_server = NULL;
  p->desktop_id = NULL;
  p->skeleton = dbus_sync_menu_app_skeleton_new ();
  p->state = SYNC_MENU_STATE_IDLE;
  p->bus_get_cancellable = NULL;

  client->priv = p;
}

static void
sync_menu_app_constructed (GObject * object)
{
  GCancellable * cancellable;
  SyncMenuApp * app = SYNC_MENU_APP(object);
  SyncMenuAppPriv * p = app->priv;

  g_object_bind_property (app,         SYNC_MENU_APP_PROP_PAUSED,
                          p->skeleton, SYNC_MENU_APP_PROP_PAUSED,
                          G_BINDING_BIDIRECTIONAL|G_BINDING_SYNC_CREATE);

  g_object_bind_property (app,         SYNC_MENU_APP_PROP_STATE,
                          p->skeleton, SYNC_MENU_APP_PROP_STATE,
                          G_BINDING_SYNC_CREATE);

  cancellable = g_cancellable_new();
  g_object_set_data (G_OBJECT(cancellable), "sync-menu-app", app);
  g_bus_get (G_BUS_TYPE_SESSION, cancellable, on_got_bus, cancellable);
  p->bus_get_cancellable = cancellable;

  G_OBJECT_CLASS (sync_menu_app_parent_class)->constructed (object);
}

/**
 * sync_menu_app_new:
 * @desktop_id: A desktop id as described by g_desktop_app_info_new(),
 *              such as "transmission-gtk.desktop"
 *
 * Creates a new #SyncMenuApp object. Applications wanting to interact
 * with the Sync Indicator should instantiate one of these and use it.
 *
 * The initial state is %SYNC_MENU_STATE_IDLE, unpaused, and with no menu.
 *
 * Returns: (transfer full): a new #SyncMenuApp for the desktop id.
 *                           Free the returned object with g_object_unref().
 */
SyncMenuApp *
sync_menu_app_new (const char * desktop_id)
{
  GObject * o = g_object_new (SYNC_MENU_APP_TYPE,
                              SYNC_MENU_APP_PROP_DESKTOP_ID, desktop_id,
                              NULL);

  return SYNC_MENU_APP(o);
}

/***
****
***/

static void
get_property (GObject     * o,
              guint         prop_id,
              GValue      * value,
              GParamSpec  * pspec)
{
  SyncMenuApp * client = SYNC_MENU_APP(o);

  switch (prop_id)
    {
      case PROP_STATE:
        g_value_set_enum (value, client->priv->state);
        break;

      case PROP_PAUSED:
        g_value_set_boolean (value, client->priv->paused);
        break;

      case PROP_DESKTOP_ID:
        g_value_set_string (value, client->priv->desktop_id);
        break;

      case PROP_DBUSMENU:
        g_value_set_object (value, sync_menu_app_get_menu(client));
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(o, prop_id, pspec);
        break;
    }
}

static void
set_property (GObject       * o,
              guint           prop_id,
              const GValue  * value,
              GParamSpec    * pspec)
{
  SyncMenuApp * client = SYNC_MENU_APP(o);
  SyncMenuAppPriv * p = client->priv;

  switch (prop_id)
    {
      case PROP_STATE:
        sync_menu_app_set_state (client, g_value_get_enum(value));
        break;

      case PROP_PAUSED:
        sync_menu_app_set_paused (client, g_value_get_boolean (value));
        break;

      case PROP_DBUSMENU:
        sync_menu_app_set_menu (client, DBUSMENU_SERVER(g_value_get_object(value)));
        break;

      case PROP_DESKTOP_ID:
        g_return_if_fail (p->desktop_id == NULL); /* ctor only */
        p->desktop_id = g_value_dup_string (value);
        if (p->desktop_id == NULL)
          g_warning ("No Desktop ID found! Did you give the constructor one?");
        else
          g_debug (G_STRLOC" initializing desktop_id to '%s'", p->desktop_id);
        dbus_sync_menu_app_set_desktop (p->skeleton, p->desktop_id);
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(o, prop_id, pspec);
        break;
    }
}

/**
 * sync_menu_app_get_paused:
 * @client: a #SyncMenuApp
 *
 * Returns: the client's current 'paused' property.
 */
gboolean
sync_menu_app_get_paused (SyncMenuApp * client)
{
  g_return_val_if_fail (IS_SYNC_MENU_APP(client), FALSE);

  return client->priv->paused;
}

/**
 * sync_menu_app_get_state:
 * @client: a #SyncMenuApp
 *
 * Returns: the client's current #SyncMenuState, such as %SYNC_MENU_STATE_IDLE
 */
SyncMenuState
sync_menu_app_get_state (SyncMenuApp * client)
{
  g_return_val_if_fail (IS_SYNC_MENU_APP(client), SYNC_MENU_STATE_ERROR);

  return client->priv->state;
}

/**
 * sync_menu_app_get_desktop_id:
 * @client: a #SyncMenuApp
 *
 * Returns: (transfer none): the client's desktop id
 */
const gchar*
sync_menu_app_get_desktop_id (SyncMenuApp * client)
{
  g_return_val_if_fail (IS_SYNC_MENU_APP(client), NULL);

  return client->priv->desktop_id;
}

/**
 * sync_menu_app_get_menu:
 * @client: a #SyncMenuApp
 *
 * Returns: (transfer none): the client's #DbusmenuServer
 */
DbusmenuServer*
sync_menu_app_get_menu (SyncMenuApp * client)
{
  g_return_val_if_fail (IS_SYNC_MENU_APP(client), NULL);

  return client->priv->menu_server;
}

/**
 * sync_menu_app_set_paused:
 * @client: a #SyncMenuApp
 * @paused: a boolean of whether or not the client is paused
 *
 * Sets the client's SyncMenuApp:paused property
 */
void
sync_menu_app_set_paused (SyncMenuApp * client, gboolean paused)
{
  g_return_if_fail (IS_SYNC_MENU_APP(client));
  SyncMenuAppPriv * p = client->priv;

  if (p->paused != paused)
    {
      p->paused = paused;
      g_object_notify_by_pspec (G_OBJECT(client), properties[PROP_PAUSED]);
    }
}

/**
 * sync_menu_app_set_state:
 * @client: a #SyncMenuApp
 * @state: the client's new #SyncMenuState, such as %SYNC_MENU_STATE_IDLE
 *
 * Sets the client's SyncMenuApp:paused property
 */
void
sync_menu_app_set_state (SyncMenuApp * client, SyncMenuState state)
{
  g_return_if_fail (IS_SYNC_MENU_APP(client));
  SyncMenuAppPriv * p = client->priv;

  if (p->state != state)
    {
      p->state = state;
      g_object_notify_by_pspec (G_OBJECT(client), properties[PROP_STATE]);
    }
}

/**
 * sync_menu_app_set_menu:
 * @client: a #SyncMenuApp
 * @menu_server: a #DbusmenuServer of the menu to be exported by the #SyncMenuApp
 *
 * Sets the client's SyncMenuApp:menu-path property specifying which menu
 * to export to the sync indicator.
 */
void
sync_menu_app_set_menu (SyncMenuApp * client, DbusmenuServer * menu_server)
{
  g_return_if_fail (IS_SYNC_MENU_APP(client));
  SyncMenuAppPriv * p = client->priv;

  if (p->menu_server != menu_server)
    {
      g_clear_object (&p->menu_binding);
      g_clear_object (&p->menu_server);

      if (menu_server != NULL)
        {
          p->menu_server = g_object_ref (menu_server);
          p->menu_binding = g_object_bind_property (
                               p->menu_server, DBUSMENU_SERVER_PROP_DBUS_OBJECT,
                               p->skeleton, "menu-path",
                               G_BINDING_SYNC_CREATE);
        }

      g_object_notify_by_pspec (G_OBJECT(client), properties[PROP_DBUSMENU]);
    }
}
