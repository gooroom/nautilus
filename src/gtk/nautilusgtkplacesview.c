/* nautilusgtkplacesview.c
 *
 * Copyright (C) 2015 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <gio/gio.h>
#include <gio/gvfs.h>
#include <gtk/gtk.h>

#include "nautilusgtkplacesviewprivate.h"
#include "nautilusgtkplacesviewrowprivate.h"

/**
 * SECTION:nautilusgtkplacesview
 * @Short_description: Widget that displays persistent drives and manages mounted networks
 * @Title: NautilusGtkPlacesView
 * @See_also: #GtkFileChooser
 *
 * #NautilusGtkPlacesView is a stock widget that displays a list of persistent drives
 * such as harddisk partitions and networks.  #NautilusGtkPlacesView does not monitor
 * removable devices.
 *
 * The places view displays drives and networks, and will automatically mount
 * them when the user activates. Network addresses are stored even if they fail
 * to connect. When the connection is successful, the connected network is
 * shown at the network list.
 *
 * To make use of the places view, an application at least needs to connect
 * to the #NautilusGtkPlacesView::open-location signal. This is emitted when the user
 * selects a location to open in the view.
 */

struct _NautilusGtkPlacesViewPrivate
{
  GVolumeMonitor                *volume_monitor;
  GtkPlacesOpenFlags             open_flags;
  GtkPlacesOpenFlags             current_open_flags;

  GFile                         *server_list_file;
  GFileMonitor                  *server_list_monitor;
  GFileMonitor                  *network_monitor;

  GCancellable                  *cancellable;

  gchar                         *search_query;

  GtkWidget                     *actionbar;
  GtkWidget                     *address_entry;
  GtkWidget                     *connect_button;
  GtkWidget                     *listbox;
  GtkWidget                     *popup_menu;
  GtkWidget                     *recent_servers_listbox;
  GtkWidget                     *recent_servers_popover;
  GtkWidget                     *recent_servers_stack;
  GtkWidget                     *stack;
  GtkWidget                     *server_adresses_popover;
  GtkWidget                     *network_placeholder;
  GtkWidget                     *network_placeholder_label;

  GtkSizeGroup                  *path_size_group;
  GtkSizeGroup                  *space_size_group;

  GtkEntryCompletion            *address_entry_completion;
  GtkListStore                  *completion_store;

  GCancellable                  *networks_fetching_cancellable;

  guint                          local_only : 1;
  guint                          should_open_location : 1;
  guint                          should_pulse_entry : 1;
  guint                          entry_pulse_timeout_id;
  guint                          connecting_to_server : 1;
  guint                          mounting_volume : 1;
  guint                          unmounting_mount : 1;
  guint                          fetching_networks : 1;
  guint                          loading : 1;
  guint                          destroyed : 1;
};

static void        mount_volume                                  (NautilusGtkPlacesView *view,
                                                                  GVolume       *volume);

static gboolean    on_button_press_event                         (NautilusGtkPlacesViewRow *row,
                                                                  GdkEventButton   *event);

static void        on_eject_button_clicked                       (GtkWidget        *widget,
                                                                  NautilusGtkPlacesViewRow *row);

static gboolean    on_row_popup_menu                             (NautilusGtkPlacesViewRow *row);

static void        populate_servers                              (NautilusGtkPlacesView *view);

static gboolean    nautilus_gtk_places_view_get_fetching_networks         (NautilusGtkPlacesView *view);

static void        nautilus_gtk_places_view_set_fetching_networks         (NautilusGtkPlacesView *view,
                                                                  gboolean       fetching_networks);

static void        nautilus_gtk_places_view_set_loading                   (NautilusGtkPlacesView *view,
                                                                  gboolean       loading);

static void        update_loading                                (NautilusGtkPlacesView *view);

G_DEFINE_TYPE_WITH_PRIVATE (NautilusGtkPlacesView, nautilus_gtk_places_view, GTK_TYPE_BOX)

/* NautilusGtkPlacesView properties & signals */
enum {
  PROP_0,
  PROP_LOCAL_ONLY,
  PROP_OPEN_FLAGS,
  PROP_FETCHING_NETWORKS,
  PROP_LOADING,
  LAST_PROP
};

enum {
  OPEN_LOCATION,
  SHOW_ERROR_MESSAGE,
  LAST_SIGNAL
};

const gchar *unsupported_protocols [] =
{
  "file", "afc", "obex", "http",
  "trash", "burn", "computer",
  "archive", "recent", "localtest",
  NULL
};

static guint places_view_signals [LAST_SIGNAL] = { 0 };
static GParamSpec *properties [LAST_PROP];

static void
emit_open_location (NautilusGtkPlacesView      *view,
                    GFile              *location,
                    GtkPlacesOpenFlags  open_flags)
{
  NautilusGtkPlacesViewPrivate *priv;

  priv = nautilus_gtk_places_view_get_instance_private (view);

  if ((open_flags & priv->open_flags) == 0)
    open_flags = GTK_PLACES_OPEN_NORMAL;

  g_signal_emit (view, places_view_signals[OPEN_LOCATION], 0, location, open_flags);
}

static void
emit_show_error_message (NautilusGtkPlacesView *view,
                         gchar         *primary_message,
                         gchar         *secondary_message)
{
  g_signal_emit (view, places_view_signals[SHOW_ERROR_MESSAGE],
                         0, primary_message, secondary_message);
}

static void
server_file_changed_cb (NautilusGtkPlacesView *view)
{
  populate_servers (view);
}

static GBookmarkFile *
server_list_load (NautilusGtkPlacesView *view)
{
  NautilusGtkPlacesViewPrivate *priv;
  GBookmarkFile *bookmarks;
  GError *error = NULL;
  gchar *datadir;
  gchar *filename;

  priv = nautilus_gtk_places_view_get_instance_private (view);
  bookmarks = g_bookmark_file_new ();
  datadir = g_build_filename (g_get_user_config_dir (), "gtk-3.0", NULL);
  filename = g_build_filename (datadir, "servers", NULL);

  g_mkdir_with_parents (datadir, 0700);
  g_bookmark_file_load_from_file (bookmarks, filename, &error);

  if (error)
    {
      if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          /* only warn if the file exists */
          g_warning ("Unable to open server bookmarks: %s", error->message);
          g_clear_pointer (&bookmarks, g_bookmark_file_free);
        }

      g_clear_error (&error);
    }

  /* Monitor the file in case it's modified outside this code */
  if (!priv->server_list_monitor)
    {
      priv->server_list_file = g_file_new_for_path (filename);

      if (priv->server_list_file)
        {
          priv->server_list_monitor = g_file_monitor_file (priv->server_list_file,
                                                           G_FILE_MONITOR_NONE,
                                                           NULL,
                                                           &error);

          if (error)
            {
              g_warning ("Cannot monitor server file: %s", error->message);
              g_clear_error (&error);
            }
          else
            {
              g_signal_connect_swapped (priv->server_list_monitor,
                                        "changed",
                                        G_CALLBACK (server_file_changed_cb),
                                        view);
            }
        }

      g_clear_object (&priv->server_list_file);
    }

  g_free (datadir);
  g_free (filename);

  return bookmarks;
}

static void
server_list_save (GBookmarkFile *bookmarks)
{
  gchar *filename;

  filename = g_build_filename (g_get_user_config_dir (), "gtk-3.0", "servers", NULL);
  g_bookmark_file_to_file (bookmarks, filename, NULL);
  g_free (filename);
}

static void
server_list_add_server (NautilusGtkPlacesView *view,
                        GFile         *file)
{
  GBookmarkFile *bookmarks;
  GFileInfo *info;
  GError *error;
  gchar *title;
  gchar *uri;

  error = NULL;
  bookmarks = server_list_load (view);

  if (!bookmarks)
    return;

  uri = g_file_get_uri (file);

  info = g_file_query_info (file,
                            G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
                            G_FILE_QUERY_INFO_NONE,
                            NULL,
                            &error);
  title = g_file_info_get_attribute_as_string (info, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME);

  g_bookmark_file_set_title (bookmarks, uri, title);
  g_bookmark_file_set_visited (bookmarks, uri, -1);
  g_bookmark_file_add_application (bookmarks, uri, NULL, NULL);

  server_list_save (bookmarks);

  g_bookmark_file_free (bookmarks);
  g_clear_object (&info);
  g_free (title);
  g_free (uri);
}

static void
server_list_remove_server (NautilusGtkPlacesView *view,
                           const gchar   *uri)
{
  GBookmarkFile *bookmarks;

  bookmarks = server_list_load (view);

  if (!bookmarks)
    return;

  g_bookmark_file_remove_item (bookmarks, uri, NULL);
  server_list_save (bookmarks);

  g_bookmark_file_free (bookmarks);
}

/* Returns a toplevel GtkWindow, or NULL if none */
static GtkWindow *
get_toplevel (GtkWidget *widget)
{
  GtkWidget *toplevel;

  toplevel = gtk_widget_get_toplevel (widget);
  if (!gtk_widget_is_toplevel (toplevel))
    return NULL;
  else
    return GTK_WINDOW (toplevel);
}

static void
set_busy_cursor (NautilusGtkPlacesView *view,
                 gboolean       busy)
{
  GtkWidget *widget;
  GtkWindow *toplevel;
  GdkDisplay *display;
  GdkCursor *cursor;

  toplevel = get_toplevel (GTK_WIDGET (view));
  widget = GTK_WIDGET (toplevel);
  if (!toplevel || !gtk_widget_get_realized (widget))
    return;

  display = gtk_widget_get_display (widget);

  if (busy)
    cursor = gdk_cursor_new_from_name (display, "progress");
  else
    cursor = NULL;

  gdk_window_set_cursor (gtk_widget_get_window (widget), cursor);
  gdk_display_flush (display);

  if (cursor)
    g_object_unref (cursor);
}

/* Activates the given row, with the given flags as parameter */
static void
activate_row (NautilusGtkPlacesView      *view,
              NautilusGtkPlacesViewRow   *row,
              GtkPlacesOpenFlags  flags)
{
  NautilusGtkPlacesViewPrivate *priv;
  GVolume *volume;
  GMount *mount;
  GFile *file;

  priv = nautilus_gtk_places_view_get_instance_private (view);
  mount = nautilus_gtk_places_view_row_get_mount (row);
  volume = nautilus_gtk_places_view_row_get_volume (row);
  file = nautilus_gtk_places_view_row_get_file (row);

  if (file)
    {
      emit_open_location (view, file, flags);
    }
  else if (mount)
    {
      GFile *location = g_mount_get_default_location (mount);

      emit_open_location (view, location, flags);

      g_object_unref (location);
    }
  else if (volume && g_volume_can_mount (volume))
    {
      /*
       * When the row is activated, the unmounted volume shall
       * be mounted and opened right after.
       */
      priv->should_open_location = TRUE;

      nautilus_gtk_places_view_row_set_busy (row, TRUE);
      mount_volume (view, volume);
    }
}

static void update_places (NautilusGtkPlacesView *view);

static void
nautilus_gtk_places_view_destroy (GtkWidget *widget)
{
  NautilusGtkPlacesView *self = NAUTILUS_GTK_PLACES_VIEW (widget);
  NautilusGtkPlacesViewPrivate *priv = nautilus_gtk_places_view_get_instance_private (self);

  priv->destroyed = 1;

  g_signal_handlers_disconnect_by_func (priv->volume_monitor, update_places, widget);

  if (priv->network_monitor)
    g_signal_handlers_disconnect_by_func (priv->network_monitor, update_places, widget);

  g_cancellable_cancel (priv->cancellable);
  g_cancellable_cancel (priv->networks_fetching_cancellable);

  GTK_WIDGET_CLASS (nautilus_gtk_places_view_parent_class)->destroy (widget);
}

static void
nautilus_gtk_places_view_finalize (GObject *object)
{
  NautilusGtkPlacesView *self = (NautilusGtkPlacesView *)object;
  NautilusGtkPlacesViewPrivate *priv = nautilus_gtk_places_view_get_instance_private (self);

  if (priv->entry_pulse_timeout_id > 0)
    g_source_remove (priv->entry_pulse_timeout_id);

  g_clear_pointer (&priv->search_query, g_free);
  g_clear_object (&priv->server_list_file);
  g_clear_object (&priv->server_list_monitor);
  g_clear_object (&priv->volume_monitor);
  g_clear_object (&priv->network_monitor);
  g_clear_object (&priv->cancellable);
  g_clear_object (&priv->networks_fetching_cancellable);
  g_clear_object (&priv->path_size_group);
  g_clear_object (&priv->space_size_group);

  G_OBJECT_CLASS (nautilus_gtk_places_view_parent_class)->finalize (object);
}

static void
nautilus_gtk_places_view_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  NautilusGtkPlacesView *self = NAUTILUS_GTK_PLACES_VIEW (object);

  switch (prop_id)
    {
    case PROP_LOCAL_ONLY:
      g_value_set_boolean (value, nautilus_gtk_places_view_get_local_only (self));
      break;

    case PROP_LOADING:
      g_value_set_boolean (value, nautilus_gtk_places_view_get_loading (self));
      break;

    case PROP_FETCHING_NETWORKS:
      g_value_set_boolean (value, nautilus_gtk_places_view_get_fetching_networks (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
nautilus_gtk_places_view_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  NautilusGtkPlacesView *self = NAUTILUS_GTK_PLACES_VIEW (object);

  switch (prop_id)
    {
    case PROP_LOCAL_ONLY:
      nautilus_gtk_places_view_set_local_only (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static gboolean
is_external_volume (GVolume *volume)
{
  gboolean is_external;
  GDrive *drive;
  gchar *id;

  drive = g_volume_get_drive (volume);
  id = g_volume_get_identifier (volume, G_VOLUME_IDENTIFIER_KIND_CLASS);

  is_external = g_volume_can_eject (volume);

  /* NULL volume identifier only happens on removable devices */
  is_external |= !id;

  if (drive)
    is_external |= g_drive_is_removable (drive);

  g_clear_object (&drive);
  g_free (id);

  return is_external;
}

typedef struct
{
  gchar         *uri;
  NautilusGtkPlacesView *view;
} RemoveServerData;

static void
on_remove_server_button_clicked (RemoveServerData *data)
{
  server_list_remove_server (data->view, data->uri);

  populate_servers (data->view);
}

static void
populate_servers (NautilusGtkPlacesView *view)
{
  NautilusGtkPlacesViewPrivate *priv;
  GBookmarkFile *server_list;
  GList *children;
  gchar **uris;
  gsize num_uris;
  gint i;

  priv = nautilus_gtk_places_view_get_instance_private (view);
  server_list = server_list_load (view);

  if (!server_list)
    return;

  uris = g_bookmark_file_get_uris (server_list, &num_uris);

  gtk_stack_set_visible_child_name (GTK_STACK (priv->recent_servers_stack),
                                    num_uris > 0 ? "list" : "empty");

  if (!uris)
    {
      g_bookmark_file_free (server_list);
      return;
    }

  /* clear previous items */
  children = gtk_container_get_children (GTK_CONTAINER (priv->recent_servers_listbox));
  g_list_free_full (children, (GDestroyNotify) gtk_widget_destroy);

  gtk_list_store_clear (priv->completion_store);

  for (i = 0; i < num_uris; i++)
    {
      RemoveServerData *data;
      GtkTreeIter iter;
      GtkWidget *row;
      GtkWidget *grid;
      GtkWidget *button;
      GtkWidget *label;
      gchar *name;
      gchar *dup_uri;

      name = g_bookmark_file_get_title (server_list, uris[i], NULL);
      dup_uri = g_strdup (uris[i]);

      /* add to the completion list */
      gtk_list_store_append (priv->completion_store, &iter);
      gtk_list_store_set (priv->completion_store,
                          &iter,
                          0, name,
                          1, uris[i],
                          -1);

      /* add to the recent servers listbox */
      row = gtk_list_box_row_new ();

      grid = g_object_new (GTK_TYPE_GRID,
                           "orientation", GTK_ORIENTATION_VERTICAL,
                           "border-width", 3,
                           NULL);

      /* name of the connected uri, if any */
      label = gtk_label_new (name);
      gtk_widget_set_hexpand (label, TRUE);
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
      gtk_container_add (GTK_CONTAINER (grid), label);

      /* the uri itself */
      label = gtk_label_new (uris[i]);
      gtk_widget_set_hexpand (label, TRUE);
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
      gtk_style_context_add_class (gtk_widget_get_style_context (label), "dim-label");
      gtk_container_add (GTK_CONTAINER (grid), label);

      /* remove button */
      button = gtk_button_new_from_icon_name ("window-close-symbolic", GTK_ICON_SIZE_BUTTON);
      gtk_widget_set_halign (button, GTK_ALIGN_END);
      gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
      gtk_button_set_relief (GTK_BUTTON (button), GTK_RELIEF_NONE);
      gtk_style_context_add_class (gtk_widget_get_style_context (button), "sidebar-button");
      gtk_grid_attach (GTK_GRID (grid), button, 1, 0, 1, 2);

      gtk_container_add (GTK_CONTAINER (row), grid);
      gtk_container_add (GTK_CONTAINER (priv->recent_servers_listbox), row);

      /* custom data */
      data = g_new0 (RemoveServerData, 1);
      data->view = view;
      data->uri = dup_uri;

      g_object_set_data_full (G_OBJECT (row), "uri", dup_uri, g_free);
      g_object_set_data_full (G_OBJECT (row), "remove-server-data", data, g_free);

      g_signal_connect_swapped (button,
                                "clicked",
                                G_CALLBACK (on_remove_server_button_clicked),
                                data);

      gtk_widget_show_all (row);

      g_free (name);
    }

  g_strfreev (uris);
  g_bookmark_file_free (server_list);
}

static void
update_view_mode (NautilusGtkPlacesView *view)
{
  NautilusGtkPlacesViewPrivate *priv;
  GList *children;
  GList *l;
  gboolean show_listbox;

  priv = nautilus_gtk_places_view_get_instance_private (view);
  show_listbox = FALSE;

  /* drives */
  children = gtk_container_get_children (GTK_CONTAINER (priv->listbox));

  for (l = children; l; l = l->next)
    {
      /* GtkListBox filter rows by changing their GtkWidget::child-visible property */
      if (gtk_widget_get_child_visible (l->data))
        {
          show_listbox = TRUE;
          break;
        }
    }

  g_list_free (children);

  if (!show_listbox &&
      priv->search_query &&
      priv->search_query[0] != '\0')
    {
        gtk_stack_set_visible_child_name (GTK_STACK (priv->stack), "empty-search");
    }
  else
    {
      gtk_stack_set_visible_child_name (GTK_STACK (priv->stack), "browse");
    }
}

static void
insert_row (NautilusGtkPlacesView *view,
            GtkWidget     *row,
            gboolean       is_network)
{
  NautilusGtkPlacesViewPrivate *priv;

  priv = nautilus_gtk_places_view_get_instance_private (view);

  g_object_set_data (G_OBJECT (row), "is-network", GINT_TO_POINTER (is_network));

  g_signal_connect_swapped (nautilus_gtk_places_view_row_get_event_box (NAUTILUS_GTK_PLACES_VIEW_ROW (row)),
                            "button-press-event",
                            G_CALLBACK (on_button_press_event),
                            row);

  g_signal_connect (row,
                    "popup-menu",
                    G_CALLBACK (on_row_popup_menu),
                    row);

  g_signal_connect (nautilus_gtk_places_view_row_get_eject_button (NAUTILUS_GTK_PLACES_VIEW_ROW (row)),
                    "clicked",
                    G_CALLBACK (on_eject_button_clicked),
                    row);

  nautilus_gtk_places_view_row_set_path_size_group (NAUTILUS_GTK_PLACES_VIEW_ROW (row), priv->path_size_group);
  nautilus_gtk_places_view_row_set_space_size_group (NAUTILUS_GTK_PLACES_VIEW_ROW (row), priv->space_size_group);

  gtk_container_add (GTK_CONTAINER (priv->listbox), row);
}

static void
add_volume (NautilusGtkPlacesView *view,
            GVolume       *volume)
{
  gboolean is_network;
  GMount *mount;
  GFile *root;
  GIcon *icon;
  gchar *identifier;
  gchar *name;
  gchar *path;

  if (is_external_volume (volume))
    return;

  identifier = g_volume_get_identifier (volume, G_VOLUME_IDENTIFIER_KIND_CLASS);
  is_network = g_strcmp0 (identifier, "network") == 0;

  mount = g_volume_get_mount (volume);
  root = mount ? g_mount_get_default_location (mount) : NULL;
  icon = g_volume_get_icon (volume);
  name = g_volume_get_name (volume);
  path = !is_network ? g_volume_get_identifier (volume, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE) : NULL;

  if (!mount || !g_mount_is_shadowed (mount))
    {
      GtkWidget *row;

      row = g_object_new (NAUTILUS_TYPE_GTK_PLACES_VIEW_ROW,
                          "icon", icon,
                          "name", name,
                          "path", path ? path : "",
                          "volume", volume,
                          "mount", mount,
                          "file", NULL,
                          "is-network", is_network,
                          NULL);

      insert_row (view, row, is_network);
    }

  g_clear_object (&root);
  g_clear_object (&icon);
  g_clear_object (&mount);
  g_free (identifier);
  g_free (name);
  g_free (path);
}

static void
add_mount (NautilusGtkPlacesView *view,
           GMount        *mount)
{
  gboolean is_network;
  GFile *root;
  GIcon *icon;
  gchar *name;
  gchar *path;
  gchar *uri;
  gchar *schema;

  icon = g_mount_get_icon (mount);
  name = g_mount_get_name (mount);
  root = g_mount_get_default_location (mount);
  path = root ? g_file_get_parse_name (root) : NULL;
  uri = g_file_get_uri (root);
  schema = g_uri_parse_scheme (uri);
  is_network = g_strcmp0 (schema, "file") != 0;

  if (is_network)
    g_clear_pointer (&path, g_free);

  if (!g_mount_is_shadowed (mount))
    {
      GtkWidget *row;

      row = g_object_new (NAUTILUS_TYPE_GTK_PLACES_VIEW_ROW,
                          "icon", icon,
                          "name", name,
                          "path", path ? path : "",
                          "volume", NULL,
                          "mount", mount,
                          "file", NULL,
                          "is-network", is_network,
                          NULL);

      insert_row (view, row, is_network);
    }

  g_clear_object (&root);
  g_clear_object (&icon);
  g_free (name);
  g_free (path);
  g_free (uri);
  g_free (schema);
}

static void
add_drive (NautilusGtkPlacesView *view,
           GDrive        *drive)
{
  GList *volumes;
  GList *l;

  volumes = g_drive_get_volumes (drive);

  for (l = volumes; l != NULL; l = l->next)
    add_volume (view, l->data);

  g_list_free_full (volumes, g_object_unref);
}

static void
add_file (NautilusGtkPlacesView *view,
          GFile         *file,
          GIcon         *icon,
          const gchar   *display_name,
          const gchar   *path,
          gboolean       is_network)
{
  GtkWidget *row;
  row = g_object_new (NAUTILUS_TYPE_GTK_PLACES_VIEW_ROW,
                      "icon", icon,
                      "name", display_name,
                      "path", path,
                      "volume", NULL,
                      "mount", NULL,
                      "file", file,
                      "is_network", is_network,
                      NULL);

  insert_row (view, row, is_network);
}

static gboolean
has_networks (NautilusGtkPlacesView *view)
{
  GList *l;
  NautilusGtkPlacesViewPrivate *priv;
  GList *children;
  gboolean has_network = FALSE;

  priv = nautilus_gtk_places_view_get_instance_private (view);

  children = gtk_container_get_children (GTK_CONTAINER (priv->listbox));
  for (l = children; l != NULL; l = l->next)
    {
      if (GPOINTER_TO_INT (g_object_get_data (l->data, "is-network")) == TRUE &&
          g_object_get_data (l->data, "is-placeholder") == NULL)
      {
        has_network = TRUE;
        break;
      }
    }

  g_list_free (children);

  return has_network;
}

static void
update_network_state (NautilusGtkPlacesView *view)
{
  NautilusGtkPlacesViewPrivate *priv;

  priv = nautilus_gtk_places_view_get_instance_private (view);

  if (priv->network_placeholder == NULL)
    {
      priv->network_placeholder = gtk_list_box_row_new ();
      priv->network_placeholder_label = gtk_label_new ("");
      gtk_label_set_xalign (GTK_LABEL (priv->network_placeholder_label), 0.0);
      gtk_widget_set_margin_start (priv->network_placeholder_label, 12);
      gtk_widget_set_margin_end (priv->network_placeholder_label, 12);
      gtk_widget_set_margin_top (priv->network_placeholder_label, 6);
      gtk_widget_set_margin_bottom (priv->network_placeholder_label, 6);
      gtk_widget_set_hexpand (priv->network_placeholder_label, TRUE);
      gtk_widget_set_sensitive (priv->network_placeholder, FALSE);
      gtk_container_add (GTK_CONTAINER (priv->network_placeholder),
                         priv->network_placeholder_label);
      g_object_set_data (G_OBJECT (priv->network_placeholder),
                         "is-network", GINT_TO_POINTER (TRUE));
      /* mark the row as placeholder, so it always goes first */
      g_object_set_data (G_OBJECT (priv->network_placeholder),
                         "is-placeholder", GINT_TO_POINTER (TRUE));
      gtk_container_add (GTK_CONTAINER (priv->listbox), priv->network_placeholder);
    }

  if (nautilus_gtk_places_view_get_fetching_networks (view))
    {
      /* only show a placeholder with a message if the list is empty.
       * otherwise just show the spinner in the header */
      if (!has_networks (view))
        {
          gtk_widget_show_all (priv->network_placeholder);
          gtk_label_set_text (GTK_LABEL (priv->network_placeholder_label),
                              _("Searching for network locations"));
        }
    }
  else if (!has_networks (view))
    {
      gtk_widget_show_all (priv->network_placeholder);
      gtk_label_set_text (GTK_LABEL (priv->network_placeholder_label),
                          _("No network locations found"));
    }
  else
    {
      gtk_widget_hide (priv->network_placeholder);
    }
}

static void
monitor_network (NautilusGtkPlacesView *self)
{
  NautilusGtkPlacesViewPrivate *priv;
  GFile *network_file;
  GError *error;

  priv = nautilus_gtk_places_view_get_instance_private (self);

  if (priv->network_monitor)
    return;

  error = NULL;
  network_file = g_file_new_for_uri ("network:///");
  priv->network_monitor = g_file_monitor (network_file,
                                          G_FILE_MONITOR_NONE,
                                          NULL,
                                          &error);

  g_clear_object (&network_file);

  if (error)
    {
      g_warning ("Error monitoring network: %s", error->message);
      g_clear_error (&error);
      return;
    }

  g_signal_connect_swapped (priv->network_monitor,
                            "changed",
                            G_CALLBACK (update_places),
                            self);
}

static void
populate_networks (NautilusGtkPlacesView   *view,
                   GFileEnumerator *enumerator,
                   GList           *detected_networks)
{
  GList *l;
  GFile *file;
  GFile *activatable_file;
  gchar *uri;
  GFileType type;
  GIcon *icon;
  gchar *display_name;

  for (l = detected_networks; l != NULL; l = l->next)
    {
      file = g_file_enumerator_get_child (enumerator, l->data);
      type = g_file_info_get_file_type (l->data);
      if (type == G_FILE_TYPE_SHORTCUT || type == G_FILE_TYPE_MOUNTABLE)
        uri = g_file_info_get_attribute_as_string (l->data, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI);
      else
        uri = g_file_get_uri (file);
      activatable_file = g_file_new_for_uri (uri);
      display_name = g_file_info_get_attribute_as_string (l->data, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME);
      icon = g_file_info_get_icon (l->data);

      add_file (view, activatable_file, icon, display_name, NULL, TRUE);

      g_free (uri);
      g_free (display_name);
      g_clear_object (&file);
      g_clear_object (&activatable_file);
    }
}

static void
network_enumeration_next_files_finished (GObject      *source_object,
                                         GAsyncResult *res,
                                         gpointer      user_data)
{
  NautilusGtkPlacesViewPrivate *priv;
  NautilusGtkPlacesView *view;
  GList *detected_networks;
  GError *error;

  view = NAUTILUS_GTK_PLACES_VIEW (user_data);
  priv = nautilus_gtk_places_view_get_instance_private (view);
  error = NULL;

  detected_networks = g_file_enumerator_next_files_finish (G_FILE_ENUMERATOR (source_object),
                                                           res, &error);

  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to fetch network locations: %s", error->message);

      g_clear_error (&error);
    }
  else
    {
      nautilus_gtk_places_view_set_fetching_networks (view, FALSE);
      populate_networks (view, G_FILE_ENUMERATOR (source_object), detected_networks);

      g_list_free_full (detected_networks, g_object_unref);
    }

  g_object_unref (view);

  /* avoid to update widgets if we are already destroyed
     (and got cancelled s a result of that) */
  if (!priv->destroyed)
    {
      update_network_state (view);
      monitor_network (view);
      update_loading (view);
    }
}

static void
network_enumeration_finished (GObject      *source_object,
                              GAsyncResult *res,
                              gpointer      user_data)
{
  NautilusGtkPlacesViewPrivate *priv;
  GFileEnumerator *enumerator;
  GError *error;

  error = NULL;
  enumerator = g_file_enumerate_children_finish (G_FILE (source_object), res, &error);

  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
          !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
        g_warning ("Failed to fetch network locations: %s", error->message);

      g_clear_error (&error);
      g_object_unref (NAUTILUS_GTK_PLACES_VIEW (user_data));
    }
  else
    {
      priv = nautilus_gtk_places_view_get_instance_private (NAUTILUS_GTK_PLACES_VIEW (user_data));
      g_file_enumerator_next_files_async (enumerator,
                                          G_MAXINT32,
                                          G_PRIORITY_DEFAULT,
                                          priv->networks_fetching_cancellable,
                                          network_enumeration_next_files_finished,
                                          user_data);
      g_object_unref (enumerator);
    }
}

static void
fetch_networks (NautilusGtkPlacesView *view)
{
  NautilusGtkPlacesViewPrivate *priv;
  GFile *network_file;
  const gchar * const *supported_uris;
  gboolean found;

  priv = nautilus_gtk_places_view_get_instance_private (view);
  supported_uris = g_vfs_get_supported_uri_schemes (g_vfs_get_default ());

  for (found = FALSE; !found && supported_uris && supported_uris[0]; supported_uris++)
    if (g_strcmp0 (supported_uris[0], "network") == 0)
      found = TRUE;

  if (!found)
    return;

  network_file = g_file_new_for_uri ("network:///");

  g_cancellable_cancel (priv->networks_fetching_cancellable);
  g_clear_object (&priv->networks_fetching_cancellable);
  priv->networks_fetching_cancellable = g_cancellable_new ();
  nautilus_gtk_places_view_set_fetching_networks (view, TRUE);
  update_network_state (view);

  g_object_ref (view);
  g_file_enumerate_children_async (network_file,
                                   "standard::type,standard::target-uri,standard::name,standard::display-name,standard::icon",
                                   G_FILE_QUERY_INFO_NONE,
                                   G_PRIORITY_DEFAULT,
                                   priv->networks_fetching_cancellable,
                                   network_enumeration_finished,
                                   view);

  g_clear_object (&network_file);
}

static void
update_places (NautilusGtkPlacesView *view)
{
  NautilusGtkPlacesViewPrivate *priv;
  GList *children;
  GList *mounts;
  GList *volumes;
  GList *drives;
  GList *l;
  GIcon *icon;
  GFile *file;

  priv = nautilus_gtk_places_view_get_instance_private (view);

  /* Clear all previously added items */
  children = gtk_container_get_children (GTK_CONTAINER (priv->listbox));
  g_list_free_full (children, (GDestroyNotify) gtk_widget_destroy);
  priv->network_placeholder = NULL;
  /* Inform clients that we started loading */
  nautilus_gtk_places_view_set_loading (view, TRUE);

  /* Add "Computer" row */
  file = g_file_new_for_path ("/");
  icon = g_themed_icon_new_with_default_fallbacks ("drive-harddisk");

  add_file (view, file, icon, _("Computer"), "/", FALSE);

  g_clear_object (&file);
  g_clear_object (&icon);

  /* Add currently connected drives */
  drives = g_volume_monitor_get_connected_drives (priv->volume_monitor);

  for (l = drives; l != NULL; l = l->next)
    add_drive (view, l->data);

  g_list_free_full (drives, g_object_unref);

  /*
   * Since all volumes with an associated GDrive were already added with
   * add_drive before, add all volumes that aren't associated with a
   * drive.
   */
  volumes = g_volume_monitor_get_volumes (priv->volume_monitor);

  for (l = volumes; l != NULL; l = l->next)
    {
      GVolume *volume;
      GDrive *drive;

      volume = l->data;
      drive = g_volume_get_drive (volume);

      if (drive)
        {
          g_object_unref (drive);
          continue;
        }

      add_volume (view, volume);
    }

  g_list_free_full (volumes, g_object_unref);

  /*
   * Now that all necessary drives and volumes were already added, add mounts
   * that have no volume, such as /etc/mtab mounts, ftp, sftp, etc.
   */
  mounts = g_volume_monitor_get_mounts (priv->volume_monitor);

  for (l = mounts; l != NULL; l = l->next)
    {
      GMount *mount;
      GVolume *volume;

      mount = l->data;
      volume = g_mount_get_volume (mount);

      if (volume)
        {
          g_object_unref (volume);
          continue;
        }

      add_mount (view, mount);
    }

  g_list_free_full (mounts, g_object_unref);

  /* load saved servers */
  populate_servers (view);

  /* fetch networks and add them asynchronously */
  if (!nautilus_gtk_places_view_get_local_only (view))
    fetch_networks (view);

  update_view_mode (view);
  /* Check whether we still are in a loading state */
  update_loading (view);
}

static void
server_mount_ready_cb (GObject      *source_file,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  NautilusGtkPlacesViewPrivate *priv;
  NautilusGtkPlacesView *view;
  gboolean should_show;
  GError *error;
  GFile *location;

  location = G_FILE (source_file);
  should_show = TRUE;
  error = NULL;

  view = NAUTILUS_GTK_PLACES_VIEW (user_data);

  g_file_mount_enclosing_volume_finish (location, res, &error);
  if (error)
    {
      should_show = FALSE;

      if (error->code == G_IO_ERROR_ALREADY_MOUNTED)
        {
          /*
           * Already mounted volume is not a critical error
           * and we can still continue with the operation.
           */
          should_show = TRUE;
        }
      else if (error->domain != G_IO_ERROR ||
               (error->code != G_IO_ERROR_CANCELLED &&
                error->code != G_IO_ERROR_FAILED_HANDLED))
        {
          /* if it wasn't cancelled show a dialog */
          emit_show_error_message (view, _("Unable to access location"), error->message);
        }

      /* The operation got cancelled by the user and or the error
         has been handled already. */
      g_clear_error (&error);
    }

  priv = nautilus_gtk_places_view_get_instance_private (view);

  if (priv->destroyed) {
    g_object_unref (view);
    return;
  }

  priv->should_pulse_entry = FALSE;

  /* Restore from Cancel to Connect */
  gtk_button_set_label (GTK_BUTTON (priv->connect_button), _("Con_nect"));
  gtk_widget_set_sensitive (priv->address_entry, TRUE);
  priv->connecting_to_server = FALSE;

  if (should_show)
    {
      server_list_add_server (view, location);

      /*
       * Only clear the entry if it successfully connects to the server.
       * Otherwise, the user would lost the typed address even if it fails
       * to connect.
       */
      gtk_entry_set_text (GTK_ENTRY (priv->address_entry), "");

      if (priv->should_open_location)
        emit_open_location (view, location, priv->open_flags);
    }

  update_places (view);
  g_object_unref (view);
}

static void
volume_mount_ready_cb (GObject      *source_volume,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  NautilusGtkPlacesViewPrivate *priv;
  NautilusGtkPlacesView *view;
  gboolean should_show;
  GVolume *volume;
  GError *error;

  volume = G_VOLUME (source_volume);
  should_show = TRUE;
  error = NULL;

  g_volume_mount_finish (volume, res, &error);

  if (error)
    {
      should_show = FALSE;

      if (error->code == G_IO_ERROR_ALREADY_MOUNTED)
        {
          /*
           * If the volume was already mounted, it's not a hard error
           * and we can still continue with the operation.
           */
          should_show = TRUE;
        }
      else if (error->domain != G_IO_ERROR ||
               (error->code != G_IO_ERROR_CANCELLED &&
                error->code != G_IO_ERROR_FAILED_HANDLED))
        {
          /* if it wasn't cancelled show a dialog */
          emit_show_error_message (NAUTILUS_GTK_PLACES_VIEW (user_data), _("Unable to access location"), error->message);
          should_show = FALSE;
        }

      /* The operation got cancelled by the user and or the error
         has been handled already. */
      g_clear_error (&error);
    }

  view = NAUTILUS_GTK_PLACES_VIEW (user_data);
  priv = nautilus_gtk_places_view_get_instance_private (view);

  if (priv->destroyed)
    {
      g_object_unref(view);
      return;
    }

  priv->mounting_volume = FALSE;
  update_loading (view);

  if (should_show)
    {
      GMount *mount;
      GFile *root;

      mount = g_volume_get_mount (volume);
      root = g_mount_get_default_location (mount);

      if (priv->should_open_location)
        emit_open_location (NAUTILUS_GTK_PLACES_VIEW (user_data), root, priv->open_flags);

      g_object_unref (mount);
      g_object_unref (root);
    }

  update_places (view);
  g_object_unref (view);
}

static void
unmount_ready_cb (GObject      *source_mount,
                  GAsyncResult *res,
                  gpointer      user_data)
{
  NautilusGtkPlacesView *view;
  NautilusGtkPlacesViewPrivate *priv;
  GMount *mount;
  GError *error;

  view = NAUTILUS_GTK_PLACES_VIEW (user_data);
  mount = G_MOUNT (source_mount);
  error = NULL;

  g_mount_unmount_with_operation_finish (mount, res, &error);

  if (error)
    {
      if (error->domain != G_IO_ERROR ||
          (error->code != G_IO_ERROR_CANCELLED &&
           error->code != G_IO_ERROR_FAILED_HANDLED))
        {
          /* if it wasn't cancelled show a dialog */
          emit_show_error_message (view, _("Unable to unmount volume"), error->message);
        }

      g_clear_error (&error);
    }

  priv = nautilus_gtk_places_view_get_instance_private (view);

  if (priv->destroyed) {
    g_object_unref (view);
    return;
  }

  priv->unmounting_mount = FALSE;
  update_loading (view);

  g_object_unref (view);
}

static gboolean
pulse_entry_cb (gpointer user_data)
{
  NautilusGtkPlacesViewPrivate *priv;

  priv = nautilus_gtk_places_view_get_instance_private (NAUTILUS_GTK_PLACES_VIEW (user_data));

  if (priv->destroyed)
    {
      priv->entry_pulse_timeout_id = 0;

      return G_SOURCE_REMOVE;
    }
  else if (priv->should_pulse_entry)
    {
      gtk_entry_progress_pulse (GTK_ENTRY (priv->address_entry));

      return G_SOURCE_CONTINUE;
    }
  else
    {
      gtk_entry_set_progress_pulse_step (GTK_ENTRY (priv->address_entry), 0.0);
      gtk_entry_set_progress_fraction (GTK_ENTRY (priv->address_entry), 0.0);

      return G_SOURCE_REMOVE;
    }
}

static void
unmount_mount (NautilusGtkPlacesView *view,
               GMount        *mount)
{
  NautilusGtkPlacesViewPrivate *priv;
  GMountOperation *operation;
  GtkWidget *toplevel;

  priv = nautilus_gtk_places_view_get_instance_private (view);
  toplevel = gtk_widget_get_toplevel (GTK_WIDGET (view));
  operation = gtk_mount_operation_new (GTK_WINDOW (toplevel));

  g_cancellable_cancel (priv->cancellable);
  g_clear_object (&priv->cancellable);
  priv->cancellable = g_cancellable_new ();

  priv->unmounting_mount = TRUE;
  update_loading (view);

  g_object_ref (view);

  operation = gtk_mount_operation_new (GTK_WINDOW (toplevel));
  g_mount_unmount_with_operation (mount,
                                  0,
                                  operation,
                                  priv->cancellable,
                                  unmount_ready_cb,
                                  view);
  g_object_unref (operation);
}

static void
mount_server (NautilusGtkPlacesView *view,
              GFile         *location)
{
  NautilusGtkPlacesViewPrivate *priv;
  GMountOperation *operation;
  GtkWidget *toplevel;

  priv = nautilus_gtk_places_view_get_instance_private (view);

  g_cancellable_cancel (priv->cancellable);
  g_clear_object (&priv->cancellable);
  /* User cliked when the operation was ongoing, so wanted to cancel it */
  if (priv->connecting_to_server)
    return;

  priv->cancellable = g_cancellable_new ();
  toplevel = gtk_widget_get_toplevel (GTK_WIDGET (view));
  operation = gtk_mount_operation_new (GTK_WINDOW (toplevel));

  priv->should_pulse_entry = TRUE;
  gtk_entry_set_progress_pulse_step (GTK_ENTRY (priv->address_entry), 0.1);
  /* Allow to cancel the operation */
  gtk_button_set_label (GTK_BUTTON (priv->connect_button), _("Cance_l"));
  gtk_widget_set_sensitive (priv->address_entry, FALSE);
  priv->connecting_to_server = TRUE;
  update_loading (view);

  if (priv->entry_pulse_timeout_id == 0)
    priv->entry_pulse_timeout_id = g_timeout_add (100, (GSourceFunc) pulse_entry_cb, view);

  g_mount_operation_set_password_save (operation, G_PASSWORD_SAVE_FOR_SESSION);

  /* make sure we keep the view around for as long as we are running */
  g_object_ref (view);

  g_file_mount_enclosing_volume (location,
                                 0,
                                 operation,
                                 priv->cancellable,
                                 server_mount_ready_cb,
                                 view);

  /* unref operation here - g_file_mount_enclosing_volume() does ref for itself */
  g_object_unref (operation);
}

static void
mount_volume (NautilusGtkPlacesView *view,
              GVolume       *volume)
{
  NautilusGtkPlacesViewPrivate *priv;
  GMountOperation *operation;
  GtkWidget *toplevel;

  priv = nautilus_gtk_places_view_get_instance_private (view);
  toplevel = gtk_widget_get_toplevel (GTK_WIDGET (view));
  operation = gtk_mount_operation_new (GTK_WINDOW (toplevel));

  g_cancellable_cancel (priv->cancellable);
  g_clear_object (&priv->cancellable);
  priv->cancellable = g_cancellable_new ();

  priv->mounting_volume = TRUE;
  update_loading (view);

  g_mount_operation_set_password_save (operation, G_PASSWORD_SAVE_FOR_SESSION);

  /* make sure we keep the view around for as long as we are running */
  g_object_ref (view);

  g_volume_mount (volume,
                  0,
                  operation,
                  priv->cancellable,
                  volume_mount_ready_cb,
                  view);

  /* unref operation here - g_file_mount_enclosing_volume() does ref for itself */
  g_object_unref (operation);
}

/* Callback used when the file list's popup menu is detached */
static void
popup_menu_detach_cb (GtkWidget *attach_widget,
                      GtkMenu   *menu)
{
  NautilusGtkPlacesViewPrivate *priv;

  priv = nautilus_gtk_places_view_get_instance_private (NAUTILUS_GTK_PLACES_VIEW (attach_widget));
  priv->popup_menu = NULL;
}

static void
open_cb (GtkMenuItem      *item,
         NautilusGtkPlacesViewRow *row)
{
  NautilusGtkPlacesView *self;

  self = NAUTILUS_GTK_PLACES_VIEW (gtk_widget_get_ancestor (GTK_WIDGET (row), NAUTILUS_TYPE_GTK_PLACES_VIEW));
  activate_row (self, row, GTK_PLACES_OPEN_NORMAL);
}

static void
open_in_new_tab_cb (GtkMenuItem      *item,
                    NautilusGtkPlacesViewRow *row)
{
  NautilusGtkPlacesView *self;

  self = NAUTILUS_GTK_PLACES_VIEW (gtk_widget_get_ancestor (GTK_WIDGET (row), NAUTILUS_TYPE_GTK_PLACES_VIEW));
  activate_row (self, row, GTK_PLACES_OPEN_NEW_TAB);
}

static void
open_in_new_window_cb (GtkMenuItem      *item,
                       NautilusGtkPlacesViewRow *row)
{
  NautilusGtkPlacesView *self;

  self = NAUTILUS_GTK_PLACES_VIEW (gtk_widget_get_ancestor (GTK_WIDGET (row), NAUTILUS_TYPE_GTK_PLACES_VIEW));
  activate_row (self, row, GTK_PLACES_OPEN_NEW_WINDOW);
}

static void
mount_cb (GtkMenuItem      *item,
          NautilusGtkPlacesViewRow *row)
{
  NautilusGtkPlacesViewPrivate *priv;
  GtkWidget *view;
  GVolume *volume;

  view = gtk_widget_get_ancestor (GTK_WIDGET (row), NAUTILUS_TYPE_GTK_PLACES_VIEW);
  priv = nautilus_gtk_places_view_get_instance_private (NAUTILUS_GTK_PLACES_VIEW (view));
  volume = nautilus_gtk_places_view_row_get_volume (row);

  /*
   * When the mount item is activated, it's expected that
   * the volume only gets mounted, without opening it after
   * the operation is complete.
   */
  priv->should_open_location = FALSE;

  nautilus_gtk_places_view_row_set_busy (row, TRUE);
  mount_volume (NAUTILUS_GTK_PLACES_VIEW (view), volume);
}

static void
unmount_cb (GtkMenuItem      *item,
            NautilusGtkPlacesViewRow *row)
{
  GtkWidget *view;
  GMount *mount;

  view = gtk_widget_get_ancestor (GTK_WIDGET (row), NAUTILUS_TYPE_GTK_PLACES_VIEW);
  mount = nautilus_gtk_places_view_row_get_mount (row);

  nautilus_gtk_places_view_row_set_busy (row, TRUE);

  unmount_mount (NAUTILUS_GTK_PLACES_VIEW (view), mount);
}

/* Constructs the popup menu if needed */
static void
build_popup_menu (NautilusGtkPlacesView    *view,
                  NautilusGtkPlacesViewRow *row)
{
  NautilusGtkPlacesViewPrivate *priv;
  GtkWidget *item;
  GMount *mount;
  GFile *file;
  gboolean is_network;

  priv = nautilus_gtk_places_view_get_instance_private (view);
  mount = nautilus_gtk_places_view_row_get_mount (row);
  file = nautilus_gtk_places_view_row_get_file (row);
  is_network = nautilus_gtk_places_view_row_get_is_network (row);

  priv->popup_menu = gtk_menu_new ();
  gtk_style_context_add_class (gtk_widget_get_style_context (priv->popup_menu),
                               GTK_STYLE_CLASS_CONTEXT_MENU);

  gtk_menu_attach_to_widget (GTK_MENU (priv->popup_menu),
                             GTK_WIDGET (view),
                             popup_menu_detach_cb);

  /* Open item is always present */
  item = gtk_menu_item_new_with_mnemonic (_("_Open"));
  g_signal_connect (item,
                    "activate",
                    G_CALLBACK (open_cb),
                    row);
  gtk_widget_show (item);
  gtk_menu_shell_append (GTK_MENU_SHELL (priv->popup_menu), item);

  if (priv->open_flags & GTK_PLACES_OPEN_NEW_TAB)
    {
      item = gtk_menu_item_new_with_mnemonic (_("Open in New _Tab"));
      g_signal_connect (item,
                        "activate",
                        G_CALLBACK (open_in_new_tab_cb),
                        row);
      gtk_widget_show (item);
      gtk_menu_shell_append (GTK_MENU_SHELL (priv->popup_menu), item);
    }

  if (priv->open_flags & GTK_PLACES_OPEN_NEW_WINDOW)
    {
      item = gtk_menu_item_new_with_mnemonic (_("Open in New _Window"));
      g_signal_connect (item,
                        "activate",
                        G_CALLBACK (open_in_new_window_cb),
                        row);
      gtk_widget_show (item);
      gtk_menu_shell_append (GTK_MENU_SHELL (priv->popup_menu), item);
    }

  /*
   * The only item that contains a file up to now is the Computer
   * item, which cannot be mounted or unmounted.
   */
  if (file)
    return;

  /* Separator */
  item = gtk_separator_menu_item_new ();
  gtk_widget_show (item);
  gtk_menu_shell_insert (GTK_MENU_SHELL (priv->popup_menu), item, -1);

  /* Mount/Unmount items */
  if (mount)
    {
      item = gtk_menu_item_new_with_mnemonic (is_network ? _("_Disconnect") : _("_Unmount"));
      g_signal_connect (item,
                        "activate",
                        G_CALLBACK (unmount_cb),
                        row);
      gtk_widget_show (item);
      gtk_menu_shell_append (GTK_MENU_SHELL (priv->popup_menu), item);
    }
  else
    {
      item = gtk_menu_item_new_with_mnemonic (is_network ? _("_Connect") : _("_Mount"));
      g_signal_connect (item,
                        "activate",
                        G_CALLBACK (mount_cb),
                        row);
      gtk_widget_show (item);
      gtk_menu_shell_append (GTK_MENU_SHELL (priv->popup_menu), item);
    }
}

static void
popup_menu (NautilusGtkPlacesViewRow *row,
            GdkEventButton   *event)
{
  NautilusGtkPlacesViewPrivate *priv;
  GtkWidget *view;

  view = gtk_widget_get_ancestor (GTK_WIDGET (row), NAUTILUS_TYPE_GTK_PLACES_VIEW);
  priv = nautilus_gtk_places_view_get_instance_private (NAUTILUS_GTK_PLACES_VIEW (view));

  g_clear_pointer (&priv->popup_menu, gtk_widget_destroy);

  build_popup_menu (NAUTILUS_GTK_PLACES_VIEW (view), row);

  gtk_menu_popup_at_pointer (GTK_MENU (priv->popup_menu), (GdkEvent *) event);
}

static gboolean
on_row_popup_menu (NautilusGtkPlacesViewRow *row)
{
  popup_menu (row, NULL);
  return TRUE;
}

static gboolean
on_button_press_event (NautilusGtkPlacesViewRow *row,
                       GdkEventButton   *event)
{
  if (row &&
      gdk_event_triggers_context_menu ((GdkEvent*) event) &&
      event->type == GDK_BUTTON_PRESS)
    {
      popup_menu (row, event);

      return TRUE;
    }

  return FALSE;
}

static gboolean
on_key_press_event (GtkWidget     *widget,
                    GdkEventKey   *event,
                    NautilusGtkPlacesView *view)
{
  NautilusGtkPlacesViewPrivate *priv;

  priv = nautilus_gtk_places_view_get_instance_private (view);

  if (event)
    {
      guint modifiers;

      modifiers = gtk_accelerator_get_default_mod_mask ();

      if (event->keyval == GDK_KEY_Return ||
          event->keyval == GDK_KEY_KP_Enter ||
          event->keyval == GDK_KEY_ISO_Enter ||
          event->keyval == GDK_KEY_space)
        {
          GtkWidget *focus_widget;
          GtkWindow *toplevel;

          priv->current_open_flags = GTK_PLACES_OPEN_NORMAL;
          toplevel = get_toplevel (GTK_WIDGET (view));

          if (!toplevel)
            return FALSE;

          focus_widget = gtk_window_get_focus (toplevel);

          if (!NAUTILUS_IS_GTK_PLACES_VIEW_ROW (focus_widget))
            return FALSE;

          if ((event->state & modifiers) == GDK_SHIFT_MASK)
            priv->current_open_flags = GTK_PLACES_OPEN_NEW_TAB;
          else if ((event->state & modifiers) == GDK_CONTROL_MASK)
            priv->current_open_flags = GTK_PLACES_OPEN_NEW_WINDOW;

          activate_row (view, NAUTILUS_GTK_PLACES_VIEW_ROW (focus_widget), priv->current_open_flags);

          return TRUE;
        }
    }

  return FALSE;
}

static void
on_eject_button_clicked (GtkWidget        *widget,
                         NautilusGtkPlacesViewRow *row)
{
  if (row)
    {
      GtkWidget *view = gtk_widget_get_ancestor (GTK_WIDGET (row), NAUTILUS_TYPE_GTK_PLACES_VIEW);

      unmount_mount (NAUTILUS_GTK_PLACES_VIEW (view), nautilus_gtk_places_view_row_get_mount (row));
    }
}

static void
on_connect_button_clicked (NautilusGtkPlacesView *view)
{
  NautilusGtkPlacesViewPrivate *priv;
  const gchar *uri;
  GFile *file;

  priv = nautilus_gtk_places_view_get_instance_private (view);
  file = NULL;

  /*
   * Since the 'Connect' button is updated whenever the typed
   * address changes, it is sufficient to check if it's sensitive
   * or not, in order to determine if the given address is valid.
   */
  if (!gtk_widget_get_sensitive (priv->connect_button))
    return;

  uri = gtk_entry_get_text (GTK_ENTRY (priv->address_entry));

  if (uri != NULL && uri[0] != '\0')
    file = g_file_new_for_commandline_arg (uri);

  if (file)
    {
      priv->should_open_location = TRUE;

      mount_server (view, file);
    }
  else
    {
      emit_show_error_message (view, _("Unable to get remote server location"), NULL);
    }
}

static void
on_address_entry_text_changed (NautilusGtkPlacesView *view)
{
  NautilusGtkPlacesViewPrivate *priv;
  const gchar* const *supported_protocols;
  gchar *address, *scheme;
  gboolean supported;

  priv = nautilus_gtk_places_view_get_instance_private (view);
  supported = FALSE;
  supported_protocols = g_vfs_get_supported_uri_schemes (g_vfs_get_default ());
  address = g_strdup (gtk_entry_get_text (GTK_ENTRY (priv->address_entry)));
  scheme = g_uri_parse_scheme (address);

  if (!supported_protocols)
    goto out;

  if (!scheme)
    goto out;

  supported = g_strv_contains (supported_protocols, scheme) &&
              !g_strv_contains (unsupported_protocols, scheme);

out:
  gtk_widget_set_sensitive (priv->connect_button, supported);
  g_free (address);
  g_free (scheme);
}

static void
on_address_entry_show_help_pressed (NautilusGtkPlacesView        *view,
                                    GtkEntryIconPosition  icon_pos,
                                    GdkEvent             *event,
                                    GtkEntry             *entry)
{
  NautilusGtkPlacesViewPrivate *priv;
  GdkRectangle rect;

  priv = nautilus_gtk_places_view_get_instance_private (view);

  /* Setup the auxiliary popover's rectangle */
  gtk_entry_get_icon_area (GTK_ENTRY (priv->address_entry),
                           GTK_ENTRY_ICON_SECONDARY,
                           &rect);

  gtk_popover_set_pointing_to (GTK_POPOVER (priv->server_adresses_popover), &rect);
  gtk_widget_set_visible (priv->server_adresses_popover, TRUE);
}

static void
on_recent_servers_listbox_row_activated (NautilusGtkPlacesView    *view,
                                         NautilusGtkPlacesViewRow *row,
                                         GtkWidget        *listbox)
{
  NautilusGtkPlacesViewPrivate *priv;
  gchar *uri;

  priv = nautilus_gtk_places_view_get_instance_private (view);
  uri = g_object_get_data (G_OBJECT (row), "uri");

  gtk_entry_set_text (GTK_ENTRY (priv->address_entry), uri);

  gtk_widget_hide (priv->recent_servers_popover);
}

static void
on_listbox_row_activated (NautilusGtkPlacesView    *view,
                          NautilusGtkPlacesViewRow *row,
                          GtkWidget        *listbox)
{
  NautilusGtkPlacesViewPrivate *priv;

  priv = nautilus_gtk_places_view_get_instance_private (view);

  activate_row (view, row, priv->current_open_flags);
}

static gboolean
listbox_filter_func (GtkListBoxRow *row,
                     gpointer       user_data)
{
  NautilusGtkPlacesViewPrivate *priv;
  gboolean is_network;
  gboolean is_placeholder;
  gboolean retval;
  gboolean searching;
  gchar *name;
  gchar *path;

  priv = nautilus_gtk_places_view_get_instance_private (NAUTILUS_GTK_PLACES_VIEW (user_data));
  retval = FALSE;
  searching = priv->search_query && priv->search_query[0] != '\0';

  is_network = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "is-network"));
  is_placeholder = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "is-placeholder"));

  if (is_network && priv->local_only)
    return FALSE;

  if (is_placeholder && searching)
    return FALSE;

  if (!searching)
    return TRUE;

  g_object_get (row,
                "name", &name,
                "path", &path,
                NULL);

  if (name)
    retval |= strstr (name, priv->search_query) != NULL;

  if (path)
    retval |= strstr (path, priv->search_query) != NULL;

  g_free (name);
  g_free (path);

  return retval;
}

static void
listbox_header_func (GtkListBoxRow *row,
                     GtkListBoxRow *before,
                     gpointer       user_data)
{
  gboolean row_is_network;
  gchar *text;

  text = NULL;
  row_is_network = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "is-network"));

  if (!before)
    {
      text = g_strdup_printf ("<b>%s</b>", row_is_network ? _("Networks") : _("On This Computer"));
    }
  else
    {
      gboolean before_is_network;

      before_is_network = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (before), "is-network"));

      if (before_is_network != row_is_network)
        text = g_strdup_printf ("<b>%s</b>", row_is_network ? _("Networks") : _("On This Computer"));
    }

  if (text)
    {
      GtkWidget *header;
      GtkWidget *label;
      GtkWidget *separator;

      header = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
      gtk_widget_set_margin_top (header, 6);

      separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);

      label = g_object_new (GTK_TYPE_LABEL,
                            "use_markup", TRUE,
                            "margin-start", 12,
                            "label", text,
                            "xalign", 0.0f,
                            NULL);
      if (row_is_network)
        {
          GtkWidget *header_name;
          GtkWidget *network_header_spinner;

          g_object_set (label,
                        "margin-end", 6,
                        NULL);

          header_name = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
          network_header_spinner = gtk_spinner_new ();
          g_object_set (network_header_spinner,
                        "margin-end", 12,
                        NULL);
          g_object_bind_property (NAUTILUS_GTK_PLACES_VIEW (user_data),
                                  "fetching-networks",
                                  network_header_spinner,
                                  "active",
                                  G_BINDING_SYNC_CREATE);

          gtk_container_add (GTK_CONTAINER (header_name), label);
          gtk_container_add (GTK_CONTAINER (header_name), network_header_spinner);
          gtk_container_add (GTK_CONTAINER (header), header_name);
        }
      else
        {
          g_object_set (label,
                        "hexpand", TRUE,
                        "margin-end", 12,
                        NULL);
          gtk_container_add (GTK_CONTAINER (header), label);
        }

      gtk_container_add (GTK_CONTAINER (header), separator);
      gtk_widget_show_all (header);

      gtk_list_box_row_set_header (row, header);

      g_free (text);
    }
  else
    {
      gtk_list_box_row_set_header (row, NULL);
    }
}

static gint
listbox_sort_func (GtkListBoxRow *row1,
                   GtkListBoxRow *row2,
                   gpointer       user_data)
{
  gboolean row1_is_network;
  gboolean row2_is_network;
  gchar *path1;
  gchar *path2;
  gboolean *is_placeholder1;
  gboolean *is_placeholder2;
  gint retval;

  row1_is_network = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row1), "is-network"));
  row2_is_network = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row2), "is-network"));

  retval = row1_is_network - row2_is_network;

  if (retval != 0)
    return retval;

  is_placeholder1 = g_object_get_data (G_OBJECT (row1), "is-placeholder");
  is_placeholder2 = g_object_get_data (G_OBJECT (row2), "is-placeholder");

  /* we can't have two placeholders for the same section */
  g_assert (!(is_placeholder1 != NULL && is_placeholder2 != NULL));

  if (is_placeholder1)
    return -1;
  if (is_placeholder2)
    return 1;

  g_object_get (row1, "path", &path1, NULL);
  g_object_get (row2, "path", &path2, NULL);

  retval = g_utf8_collate (path1, path2);

  g_free (path1);
  g_free (path2);

  return retval;
}

static void
nautilus_gtk_places_view_constructed (GObject *object)
{
  NautilusGtkPlacesViewPrivate *priv;

  priv = nautilus_gtk_places_view_get_instance_private (NAUTILUS_GTK_PLACES_VIEW (object));

  G_OBJECT_CLASS (nautilus_gtk_places_view_parent_class)->constructed (object);

  gtk_list_box_set_sort_func (GTK_LIST_BOX (priv->listbox),
                              listbox_sort_func,
                              object,
                              NULL);
  gtk_list_box_set_filter_func (GTK_LIST_BOX (priv->listbox),
                                listbox_filter_func,
                                object,
                                NULL);
  gtk_list_box_set_header_func (GTK_LIST_BOX (priv->listbox),
                                listbox_header_func,
                                object,
                                NULL);

  /* load drives */
  update_places (NAUTILUS_GTK_PLACES_VIEW (object));

  g_signal_connect_swapped (priv->volume_monitor,
                            "mount-added",
                            G_CALLBACK (update_places),
                            object);
  g_signal_connect_swapped (priv->volume_monitor,
                            "mount-changed",
                            G_CALLBACK (update_places),
                            object);
  g_signal_connect_swapped (priv->volume_monitor,
                            "mount-removed",
                            G_CALLBACK (update_places),
                            object);
  g_signal_connect_swapped (priv->volume_monitor,
                            "volume-added",
                            G_CALLBACK (update_places),
                            object);
  g_signal_connect_swapped (priv->volume_monitor,
                            "volume-changed",
                            G_CALLBACK (update_places),
                            object);
  g_signal_connect_swapped (priv->volume_monitor,
                            "volume-removed",
                            G_CALLBACK (update_places),
                            object);
}

static void
nautilus_gtk_places_view_map (GtkWidget *widget)
{
  NautilusGtkPlacesViewPrivate *priv;

  priv = nautilus_gtk_places_view_get_instance_private (NAUTILUS_GTK_PLACES_VIEW (widget));

  gtk_entry_set_text (GTK_ENTRY (priv->address_entry), "");

  GTK_WIDGET_CLASS (nautilus_gtk_places_view_parent_class)->map (widget);
}

static void
nautilus_gtk_places_view_class_init (NautilusGtkPlacesViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = nautilus_gtk_places_view_finalize;
  object_class->constructed = nautilus_gtk_places_view_constructed;
  object_class->get_property = nautilus_gtk_places_view_get_property;
  object_class->set_property = nautilus_gtk_places_view_set_property;

  widget_class->destroy = nautilus_gtk_places_view_destroy;
  widget_class->map = nautilus_gtk_places_view_map;

  /**
   * NautilusGtkPlacesView::open-location:
   * @view: the object which received the signal.
   * @location: (type Gio.File): #GFile to which the caller should switch.
   * @open_flags: a single value from #GtkPlacesOpenFlags specifying how the @location
   * should be opened.
   *
   * The places view emits this signal when the user selects a location
   * in it. The calling application should display the contents of that
   * location; for example, a file manager should show a list of files in
   * the specified location.
   *
   * Since: 3.18
   */
  places_view_signals [OPEN_LOCATION] =
          g_signal_new ("open-location",
                        G_OBJECT_CLASS_TYPE (object_class),
                        G_SIGNAL_RUN_FIRST,
                        G_STRUCT_OFFSET (NautilusGtkPlacesViewClass, open_location),
                        NULL, NULL, NULL,
                        G_TYPE_NONE, 2,
                        G_TYPE_OBJECT,
                        GTK_TYPE_PLACES_OPEN_FLAGS);

  /**
   * NautilusGtkPlacesView::show-error-message:
   * @view: the object which received the signal.
   * @primary: primary message with a summary of the error to show.
   * @secondary: secondary message with details of the error to show.
   *
   * The places view emits this signal when it needs the calling
   * application to present an error message.  Most of these messages
   * refer to mounting or unmounting media, for example, when a drive
   * cannot be started for some reason.
   *
   * Since: 3.18
   */
  places_view_signals [SHOW_ERROR_MESSAGE] =
          g_signal_new ("show-error-message",
                        G_OBJECT_CLASS_TYPE (object_class),
                        G_SIGNAL_RUN_FIRST,
                        G_STRUCT_OFFSET (NautilusGtkPlacesViewClass, show_error_message),
                        NULL, NULL,
                        NULL,
                        G_TYPE_NONE, 2,
                        G_TYPE_STRING,
                        G_TYPE_STRING);

  properties[PROP_LOCAL_ONLY] =
          g_param_spec_boolean ("local-only",
                                "Local Only",
                                "Whether the sidebar only includes local files",
                                FALSE,
                                G_PARAM_READWRITE);

  properties[PROP_LOADING] =
          g_param_spec_boolean ("loading",
                                "Loading",
                                "Whether the view is loading locations",
                                FALSE,
                                G_PARAM_READABLE);

  properties[PROP_FETCHING_NETWORKS] =
          g_param_spec_boolean ("fetching-networks",
                                "Fetching networks",
                                "Whether the view is fetching networks",
                                FALSE,
                                G_PARAM_READABLE);

  properties[PROP_OPEN_FLAGS] =
          g_param_spec_flags ("open-flags",
                              "Open Flags",
                              "Modes in which the calling application can open locations selected in the sidebar",
                              GTK_TYPE_PLACES_OPEN_FLAGS,
                              GTK_PLACES_OPEN_NORMAL,
                              G_PARAM_READWRITE);

  g_object_class_install_properties (object_class, LAST_PROP, properties);

  /* Bind class to template */
  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/nautilus/gtk/ui/nautilusgtkplacesview.ui");

  gtk_widget_class_bind_template_child_private (widget_class, NautilusGtkPlacesView, actionbar);
  gtk_widget_class_bind_template_child_private (widget_class, NautilusGtkPlacesView, address_entry);
  gtk_widget_class_bind_template_child_private (widget_class, NautilusGtkPlacesView, address_entry_completion);
  gtk_widget_class_bind_template_child_private (widget_class, NautilusGtkPlacesView, completion_store);
  gtk_widget_class_bind_template_child_private (widget_class, NautilusGtkPlacesView, connect_button);
  gtk_widget_class_bind_template_child_private (widget_class, NautilusGtkPlacesView, listbox);
  gtk_widget_class_bind_template_child_private (widget_class, NautilusGtkPlacesView, recent_servers_listbox);
  gtk_widget_class_bind_template_child_private (widget_class, NautilusGtkPlacesView, recent_servers_popover);
  gtk_widget_class_bind_template_child_private (widget_class, NautilusGtkPlacesView, recent_servers_stack);
  gtk_widget_class_bind_template_child_private (widget_class, NautilusGtkPlacesView, stack);
  gtk_widget_class_bind_template_child_private (widget_class, NautilusGtkPlacesView, server_adresses_popover);

  gtk_widget_class_bind_template_callback (widget_class, on_address_entry_text_changed);
  gtk_widget_class_bind_template_callback (widget_class, on_address_entry_show_help_pressed);
  gtk_widget_class_bind_template_callback (widget_class, on_connect_button_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_key_press_event);
  gtk_widget_class_bind_template_callback (widget_class, on_listbox_row_activated);
  gtk_widget_class_bind_template_callback (widget_class, on_recent_servers_listbox_row_activated);

  gtk_widget_class_set_css_name (widget_class, "placesview");
}

static void
nautilus_gtk_places_view_init (NautilusGtkPlacesView *self)
{
  NautilusGtkPlacesViewPrivate *priv;

  priv = nautilus_gtk_places_view_get_instance_private (self);

  priv->volume_monitor = g_volume_monitor_get ();
  priv->open_flags = GTK_PLACES_OPEN_NORMAL;
  priv->path_size_group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
  priv->space_size_group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);

  gtk_widget_init_template (GTK_WIDGET (self));
}

/**
 * nautilus_gtk_places_view_new:
 *
 * Creates a new #NautilusGtkPlacesView widget.
 *
 * The application should connect to at least the
 * #NautilusGtkPlacesView::open-location signal to be notified
 * when the user makes a selection in the view.
 *
 * Returns: a newly created #NautilusGtkPlacesView
 *
 * Since: 3.18
 */
GtkWidget *
nautilus_gtk_places_view_new (void)
{
  return g_object_new (NAUTILUS_TYPE_GTK_PLACES_VIEW, NULL);
}

/**
 * nautilus_gtk_places_view_set_open_flags:
 * @view: a #NautilusGtkPlacesView
 * @flags: Bitmask of modes in which the calling application can open locations
 *
 * Sets the way in which the calling application can open new locations from
 * the places view.  For example, some applications only open locations
 * “directly” into their main view, while others may support opening locations
 * in a new notebook tab or a new window.
 *
 * This function is used to tell the places @view about the ways in which the
 * application can open new locations, so that the view can display (or not)
 * the “Open in new tab” and “Open in new window” menu items as appropriate.
 *
 * When the #NautilusGtkPlacesView::open-location signal is emitted, its flags
 * argument will be set to one of the @flags that was passed in
 * nautilus_gtk_places_view_set_open_flags().
 *
 * Passing 0 for @flags will cause #GTK_PLACES_OPEN_NORMAL to always be sent
 * to callbacks for the “open-location” signal.
 *
 * Since: 3.18
 */
void
nautilus_gtk_places_view_set_open_flags (NautilusGtkPlacesView      *view,
                                GtkPlacesOpenFlags  flags)
{
  NautilusGtkPlacesViewPrivate *priv;

  g_return_if_fail (NAUTILUS_IS_GTK_PLACES_VIEW (view));

  priv = nautilus_gtk_places_view_get_instance_private (view);

  if (priv->open_flags != flags)
    {
      priv->open_flags = flags;
      g_object_notify_by_pspec (G_OBJECT (view), properties[PROP_OPEN_FLAGS]);
    }
}

/**
 * nautilus_gtk_places_view_get_open_flags:
 * @view: a #GtkPlacesSidebar
 *
 * Gets the open flags.
 *
 * Returns: the #GtkPlacesOpenFlags of @view
 *
 * Since: 3.18
 */
GtkPlacesOpenFlags
nautilus_gtk_places_view_get_open_flags (NautilusGtkPlacesView *view)
{
  NautilusGtkPlacesViewPrivate *priv;

  g_return_val_if_fail (NAUTILUS_IS_GTK_PLACES_VIEW (view), 0);

  priv = nautilus_gtk_places_view_get_instance_private (view);

  return priv->open_flags;
}

/**
 * nautilus_gtk_places_view_get_search_query:
 * @view: a #NautilusGtkPlacesView
 *
 * Retrieves the current search query from @view.
 *
 * Returns: (transfer none): the current search query.
 */
const gchar*
nautilus_gtk_places_view_get_search_query (NautilusGtkPlacesView *view)
{
  NautilusGtkPlacesViewPrivate *priv;

  g_return_val_if_fail (NAUTILUS_IS_GTK_PLACES_VIEW (view), NULL);

  priv = nautilus_gtk_places_view_get_instance_private (view);

  return priv->search_query;
}

/**
 * nautilus_gtk_places_view_set_search_query:
 * @view: a #NautilusGtkPlacesView
 * @query_text: the query, or NULL.
 *
 * Sets the search query of @view. The search is immediately performed
 * once the query is set.
 */
void
nautilus_gtk_places_view_set_search_query (NautilusGtkPlacesView *view,
                                  const gchar   *query_text)
{
  NautilusGtkPlacesViewPrivate *priv;

  g_return_if_fail (NAUTILUS_IS_GTK_PLACES_VIEW (view));

  priv = nautilus_gtk_places_view_get_instance_private (view);

  if (g_strcmp0 (priv->search_query, query_text) != 0)
    {
      g_clear_pointer (&priv->search_query, g_free);
      priv->search_query = g_strdup (query_text);

      gtk_list_box_invalidate_filter (GTK_LIST_BOX (priv->listbox));
      gtk_list_box_invalidate_headers (GTK_LIST_BOX (priv->listbox));

      update_view_mode (view);
    }
}

/**
 * nautilus_gtk_places_view_get_loading:
 * @view: a #NautilusGtkPlacesView
 *
 * Returns %TRUE if the view is loading locations.
 *
 * Since: 3.18
 */
gboolean
nautilus_gtk_places_view_get_loading (NautilusGtkPlacesView *view)
{
  NautilusGtkPlacesViewPrivate *priv;

  g_return_val_if_fail (NAUTILUS_IS_GTK_PLACES_VIEW (view), FALSE);

  priv = nautilus_gtk_places_view_get_instance_private (view);

  return priv->loading;
}

static void
update_loading (NautilusGtkPlacesView *view)
{
  NautilusGtkPlacesViewPrivate *priv;
  gboolean loading;

  g_return_if_fail (NAUTILUS_IS_GTK_PLACES_VIEW (view));

  priv = nautilus_gtk_places_view_get_instance_private (view);
  loading = priv->fetching_networks || priv->connecting_to_server ||
            priv->mounting_volume || priv->unmounting_mount;

  set_busy_cursor (view, loading);
  nautilus_gtk_places_view_set_loading (view, loading);
}

static void
nautilus_gtk_places_view_set_loading (NautilusGtkPlacesView *view,
                             gboolean       loading)
{
  NautilusGtkPlacesViewPrivate *priv;

  g_return_if_fail (NAUTILUS_IS_GTK_PLACES_VIEW (view));

  priv = nautilus_gtk_places_view_get_instance_private (view);

  if (priv->loading != loading)
    {
      priv->loading = loading;
      g_object_notify_by_pspec (G_OBJECT (view), properties [PROP_LOADING]);
    }
}

static gboolean
nautilus_gtk_places_view_get_fetching_networks (NautilusGtkPlacesView *view)
{
  NautilusGtkPlacesViewPrivate *priv;

  g_return_val_if_fail (NAUTILUS_IS_GTK_PLACES_VIEW (view), FALSE);

  priv = nautilus_gtk_places_view_get_instance_private (view);

  return priv->fetching_networks;
}

static void
nautilus_gtk_places_view_set_fetching_networks (NautilusGtkPlacesView *view,
                                       gboolean       fetching_networks)
{
  NautilusGtkPlacesViewPrivate *priv;

  g_return_if_fail (NAUTILUS_IS_GTK_PLACES_VIEW (view));

  priv = nautilus_gtk_places_view_get_instance_private (view);

  if (priv->fetching_networks != fetching_networks)
    {
      priv->fetching_networks = fetching_networks;
      g_object_notify_by_pspec (G_OBJECT (view), properties [PROP_FETCHING_NETWORKS]);
    }
}

/**
 * nautilus_gtk_places_view_get_local_only:
 * @view: a #NautilusGtkPlacesView
 *
 * Returns %TRUE if only local volumes are shown, i.e. no networks
 * are displayed.
 *
 * Returns: %TRUE if only local volumes are shown, %FALSE otherwise.
 *
 * Since: 3.18
 */
gboolean
nautilus_gtk_places_view_get_local_only (NautilusGtkPlacesView *view)
{
  NautilusGtkPlacesViewPrivate *priv;

  g_return_val_if_fail (NAUTILUS_IS_GTK_PLACES_VIEW (view), FALSE);

  priv = nautilus_gtk_places_view_get_instance_private (view);

  return priv->local_only;
}

/**
 * nautilus_gtk_places_view_set_local_only:
 * @view: a #NautilusGtkPlacesView
 * @local_only: %TRUE to hide remote locations, %FALSE to show.
 *
 * Sets the #NautilusGtkPlacesView::local-only property to @local_only.
 *
 * Since: 3.18
 */
void
nautilus_gtk_places_view_set_local_only (NautilusGtkPlacesView *view,
                                gboolean       local_only)
{
  NautilusGtkPlacesViewPrivate *priv;

  g_return_if_fail (NAUTILUS_IS_GTK_PLACES_VIEW (view));

  priv = nautilus_gtk_places_view_get_instance_private (view);

  if (priv->local_only != local_only)
    {
      priv->local_only = local_only;

      gtk_widget_set_visible (priv->actionbar, !local_only);
      update_places (view);

      update_view_mode (view);

      g_object_notify_by_pspec (G_OBJECT (view), properties [PROP_LOCAL_ONLY]);
    }
}
