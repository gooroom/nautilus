/*
 * Copyright (C) 2005 Mr Jamie McCracken
 *
 * Nautilus is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * Nautilus is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; see the file COPYING.  If not,
 * see <http://www.gnu.org/licenses/>.
 *
 * Author: Jamie McCracken <jamiemcc@gnome.org>
 *
 */

#include <config.h>
#include "nautilus-search-engine-tracker.h"

#include "nautilus-global-preferences.h"
#include "nautilus-search-hit.h"
#include "nautilus-search-provider.h"
#define DEBUG_FLAG NAUTILUS_DEBUG_SEARCH
#include "nautilus-debug.h"

#include <string.h>
#include <gio/gio.h>
#include <libtracker-sparql/tracker-sparql.h>

struct NautilusSearchEngineTrackerDetails
{
    TrackerSparqlConnection *connection;
    NautilusQuery *query;

    gboolean query_pending;
    GQueue *hits_pending;

    gboolean recursive;

    GCancellable *cancellable;
};

enum
{
    PROP_0,
    PROP_RUNNING,
    LAST_PROP
};

static void nautilus_search_provider_init (NautilusSearchProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE (NautilusSearchEngineTracker,
                         nautilus_search_engine_tracker,
                         G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (NAUTILUS_TYPE_SEARCH_PROVIDER,
                                                nautilus_search_provider_init))

static void
finalize (GObject *object)
{
    NautilusSearchEngineTracker *tracker;

    tracker = NAUTILUS_SEARCH_ENGINE_TRACKER (object);

    if (tracker->details->cancellable)
    {
        g_cancellable_cancel (tracker->details->cancellable);
        g_clear_object (&tracker->details->cancellable);
    }

    g_clear_object (&tracker->details->query);
    g_clear_object (&tracker->details->connection);
    g_queue_free_full (tracker->details->hits_pending, g_object_unref);

    G_OBJECT_CLASS (nautilus_search_engine_tracker_parent_class)->finalize (object);
}

#define BATCH_SIZE 100

static void
check_pending_hits (NautilusSearchEngineTracker *tracker,
                    gboolean                     force_send)
{
    GList *hits = NULL;
    NautilusSearchHit *hit;

    DEBUG ("Tracker engine add hits");

    if (!force_send &&
        g_queue_get_length (tracker->details->hits_pending) < BATCH_SIZE)
    {
        return;
    }

    while ((hit = g_queue_pop_head (tracker->details->hits_pending)))
    {
        hits = g_list_prepend (hits, hit);
    }

    nautilus_search_provider_hits_added (NAUTILUS_SEARCH_PROVIDER (tracker), hits);
    g_list_free_full (hits, g_object_unref);
}

static void
search_finished (NautilusSearchEngineTracker *tracker,
                 GError                      *error)
{
    DEBUG ("Tracker engine finished");

    if (error == NULL)
    {
        check_pending_hits (tracker, TRUE);
    }
    else
    {
        g_queue_foreach (tracker->details->hits_pending, (GFunc) g_object_unref, NULL);
        g_queue_clear (tracker->details->hits_pending);
    }

    tracker->details->query_pending = FALSE;

    g_object_notify (G_OBJECT (tracker), "running");

    if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
        DEBUG ("Tracker engine error %s", error->message);
        nautilus_search_provider_error (NAUTILUS_SEARCH_PROVIDER (tracker), error->message);
    }
    else
    {
        nautilus_search_provider_finished (NAUTILUS_SEARCH_PROVIDER (tracker),
                                           NAUTILUS_SEARCH_PROVIDER_STATUS_NORMAL);
        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
            DEBUG ("Tracker engine finished and cancelled");
        }
        else
        {
            DEBUG ("Tracker engine finished correctly");
        }
    }

    g_object_unref (tracker);
}

static void cursor_callback (GObject      *object,
                             GAsyncResult *result,
                             gpointer      user_data);

static void
cursor_next (NautilusSearchEngineTracker *tracker,
             TrackerSparqlCursor         *cursor)
{
    tracker_sparql_cursor_next_async (cursor,
                                      tracker->details->cancellable,
                                      cursor_callback,
                                      tracker);
}

static void
cursor_callback (GObject      *object,
                 GAsyncResult *result,
                 gpointer      user_data)
{
    NautilusSearchEngineTracker *tracker;
    GError *error = NULL;
    TrackerSparqlCursor *cursor;
    NautilusSearchHit *hit;
    const char *uri;
    const char *mtime_str;
    const char *atime_str;
    GTimeVal tv;
    gdouble rank, match;
    gboolean success;
    gchar *basename;

    tracker = NAUTILUS_SEARCH_ENGINE_TRACKER (user_data);

    cursor = TRACKER_SPARQL_CURSOR (object);
    success = tracker_sparql_cursor_next_finish (cursor, result, &error);

    if (!success)
    {
        search_finished (tracker, error);

        g_clear_error (&error);
        g_clear_object (&cursor);

        return;
    }

    /* We iterate result by result, not n at a time. */
    uri = tracker_sparql_cursor_get_string (cursor, 0, NULL);
    rank = tracker_sparql_cursor_get_double (cursor, 1);
    mtime_str = tracker_sparql_cursor_get_string (cursor, 2, NULL);
    atime_str = tracker_sparql_cursor_get_string (cursor, 3, NULL);
    basename = g_path_get_basename (uri);

    hit = nautilus_search_hit_new (uri);
    match = nautilus_query_matches_string (tracker->details->query, basename);
    nautilus_search_hit_set_fts_rank (hit, rank + match);
    g_free (basename);

    if (g_time_val_from_iso8601 (mtime_str, &tv))
    {
        GDateTime *date;
        date = g_date_time_new_from_timeval_local (&tv);
        nautilus_search_hit_set_modification_time (hit, date);
        g_date_time_unref (date);
    }
    else
    {
        g_warning ("unable to parse mtime: %s", mtime_str);
    }
    if (g_time_val_from_iso8601 (atime_str, &tv))
    {
        GDateTime *date;
        date = g_date_time_new_from_timeval_local (&tv);
        nautilus_search_hit_set_access_time (hit, date);
        g_date_time_unref (date);
    }
    else
    {
        g_warning ("unable to parse atime: %s", atime_str);
    }

    g_queue_push_head (tracker->details->hits_pending, hit);
    check_pending_hits (tracker, FALSE);

    /* Get next */
    cursor_next (tracker, cursor);
}

static void
query_callback (GObject      *object,
                GAsyncResult *result,
                gpointer      user_data)
{
    NautilusSearchEngineTracker *tracker;
    TrackerSparqlConnection *connection;
    TrackerSparqlCursor *cursor;
    GError *error = NULL;

    tracker = NAUTILUS_SEARCH_ENGINE_TRACKER (user_data);

    connection = TRACKER_SPARQL_CONNECTION (object);
    cursor = tracker_sparql_connection_query_finish (connection,
                                                     result,
                                                     &error);

    if (error != NULL)
    {
        search_finished (tracker, error);
        g_error_free (error);
    }
    else
    {
        cursor_next (tracker, cursor);
    }
}

static gboolean
search_finished_idle (gpointer user_data)
{
    NautilusSearchEngineTracker *tracker = user_data;

    DEBUG ("Tracker engine finished idle");

    search_finished (tracker, NULL);

    return FALSE;
}

static void
nautilus_search_engine_tracker_start (NautilusSearchProvider *provider)
{
    NautilusSearchEngineTracker *tracker;
    gchar *query_text, *search_text, *location_uri, *downcase;
    GFile *location;
    GString *sparql;
    GList *mimetypes, *l;
    gint mime_count;
    gboolean recursive;
    GPtrArray *date_range;

    tracker = NAUTILUS_SEARCH_ENGINE_TRACKER (provider);

    if (tracker->details->query_pending)
    {
        return;
    }

    DEBUG ("Tracker engine start");
    g_object_ref (tracker);
    tracker->details->query_pending = TRUE;

    g_object_notify (G_OBJECT (provider), "running");

    if (tracker->details->connection == NULL)
    {
        g_idle_add (search_finished_idle, provider);
        return;
    }

    recursive = g_settings_get_enum (nautilus_preferences, "recursive-search") == NAUTILUS_SPEED_TRADEOFF_LOCAL_ONLY ||
                g_settings_get_enum (nautilus_preferences, "recursive-search") == NAUTILUS_SPEED_TRADEOFF_ALWAYS;
    tracker->details->recursive = recursive;

    query_text = nautilus_query_get_text (tracker->details->query);
    downcase = g_utf8_strdown (query_text, -1);
    search_text = tracker_sparql_escape_string (downcase);
    g_free (query_text);
    g_free (downcase);

    location = nautilus_query_get_location (tracker->details->query);
    location_uri = location ? g_file_get_uri (location) : NULL;
    mimetypes = nautilus_query_get_mime_types (tracker->details->query);
    mime_count = g_list_length (mimetypes);

    sparql = g_string_new ("SELECT DISTINCT nie:url(?urn) fts:rank(?urn) nfo:fileLastModified(?urn) nfo:fileLastAccessed(?urn)\n"
                           "WHERE {"
                           "  ?urn a nfo:FileDataObject;"
                           "  nfo:fileLastModified ?mtime;"
                           "  nfo:fileLastAccessed ?atime;"
                           "  tracker:available true;");

    g_string_append_printf (sparql, " fts:match '\"%s\"*'", search_text);

    if (mime_count > 0)
    {
        g_string_append (sparql, "; nie:mimeType ?mime");
    }

    g_string_append_printf (sparql, " . FILTER( ");

    if (!tracker->details->recursive)
    {
        g_string_append_printf (sparql, "tracker:uri-is-parent('%s', nie:url(?urn)) && ", location_uri);
    }
    else
    {
        g_string_append_printf (sparql, "tracker:uri-is-descendant('%s', nie:url(?urn)) && ", location_uri);
    }

    g_string_append_printf (sparql, "fn:contains(fn:lower-case(nfo:fileName(?urn)), '%s')", search_text);

    date_range = nautilus_query_get_date_range (tracker->details->query);
    if (date_range)
    {
        NautilusQuerySearchType type;
        gchar *initial_date_format;
        gchar *end_date_format;
        GDateTime *initial_date;
        GDateTime *end_date;
        GDateTime *shifted_end_date;

        initial_date = g_ptr_array_index (date_range, 0);
        end_date = g_ptr_array_index (date_range, 1);
        /* As we do for other searches, we want to make the end date inclusive.
         * For that, add a day to it */
        shifted_end_date = g_date_time_add_days (end_date, 1);

        type = nautilus_query_get_search_type (tracker->details->query);
        initial_date_format = g_date_time_format (initial_date, "%Y-%m-%dT%H:%M:%S");
        end_date_format = g_date_time_format (shifted_end_date, "%Y-%m-%dT%H:%M:%S");

        g_string_append (sparql, " && ");

        if (type == NAUTILUS_QUERY_SEARCH_TYPE_LAST_ACCESS)
        {
            g_string_append_printf (sparql, "?atime >= \"%s\"^^xsd:dateTime", initial_date_format);
            g_string_append_printf (sparql, " && ?atime <= \"%s\"^^xsd:dateTime", end_date_format);
        }
        else
        {
            g_string_append_printf (sparql, "?mtime >= \"%s\"^^xsd:dateTime", initial_date_format);
            g_string_append_printf (sparql, " && ?mtime <= \"%s\"^^xsd:dateTime", end_date_format);
        }


        g_free (initial_date_format);
        g_free (end_date_format);
        g_ptr_array_unref (date_range);
    }

    if (mime_count > 0)
    {
        g_string_append (sparql, " && (");

        for (l = mimetypes; l != NULL; l = l->next)
        {
            if (l != mimetypes)
            {
                g_string_append (sparql, " || ");
            }

            g_string_append_printf (sparql, "fn:contains(?mime, '%s')",
                                    (gchar *) l->data);
        }
        g_string_append (sparql, ")\n");
    }

    g_string_append (sparql, ")} ORDER BY DESC (fts:rank(?urn))");

    tracker->details->cancellable = g_cancellable_new ();
    tracker_sparql_connection_query_async (tracker->details->connection,
                                           sparql->str,
                                           tracker->details->cancellable,
                                           query_callback,
                                           tracker);
    g_string_free (sparql, TRUE);

    g_free (search_text);
    g_free (location_uri);
    g_list_free_full (mimetypes, g_free);
    g_object_unref (location);
}

static void
nautilus_search_engine_tracker_stop (NautilusSearchProvider *provider)
{
    NautilusSearchEngineTracker *tracker;

    tracker = NAUTILUS_SEARCH_ENGINE_TRACKER (provider);

    if (tracker->details->query_pending)
    {
        DEBUG ("Tracker engine stop");
        g_cancellable_cancel (tracker->details->cancellable);
        g_clear_object (&tracker->details->cancellable);
        tracker->details->query_pending = FALSE;

        g_object_notify (G_OBJECT (provider), "running");
    }
}

static void
nautilus_search_engine_tracker_set_query (NautilusSearchProvider *provider,
                                          NautilusQuery          *query)
{
    NautilusSearchEngineTracker *tracker;

    tracker = NAUTILUS_SEARCH_ENGINE_TRACKER (provider);

    g_object_ref (query);
    g_clear_object (&tracker->details->query);
    tracker->details->query = query;
}

static gboolean
nautilus_search_engine_tracker_is_running (NautilusSearchProvider *provider)
{
    NautilusSearchEngineTracker *tracker;

    tracker = NAUTILUS_SEARCH_ENGINE_TRACKER (provider);

    return tracker->details->query_pending;
}

static void
nautilus_search_provider_init (NautilusSearchProviderInterface *iface)
{
    iface->set_query = nautilus_search_engine_tracker_set_query;
    iface->start = nautilus_search_engine_tracker_start;
    iface->stop = nautilus_search_engine_tracker_stop;
    iface->is_running = nautilus_search_engine_tracker_is_running;
}

static void
nautilus_search_engine_tracker_get_property (GObject    *object,
                                             guint       prop_id,
                                             GValue     *value,
                                             GParamSpec *pspec)
{
    NautilusSearchProvider *self = NAUTILUS_SEARCH_PROVIDER (object);

    switch (prop_id)
    {
        case PROP_RUNNING:
        {
            g_value_set_boolean (value, nautilus_search_engine_tracker_is_running (self));
        }
        break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
nautilus_search_engine_tracker_class_init (NautilusSearchEngineTrackerClass *class)
{
    GObjectClass *gobject_class;

    gobject_class = G_OBJECT_CLASS (class);
    gobject_class->finalize = finalize;
    gobject_class->get_property = nautilus_search_engine_tracker_get_property;

    /**
     * NautilusSearchEngine::running:
     *
     * Whether the search engine is running a search.
     */
    g_object_class_override_property (gobject_class, PROP_RUNNING, "running");

    g_type_class_add_private (class, sizeof (NautilusSearchEngineTrackerDetails));
}

static void
nautilus_search_engine_tracker_init (NautilusSearchEngineTracker *engine)
{
    GError *error = NULL;

    engine->details = G_TYPE_INSTANCE_GET_PRIVATE (engine, NAUTILUS_TYPE_SEARCH_ENGINE_TRACKER,
                                                   NautilusSearchEngineTrackerDetails);
    engine->details->hits_pending = g_queue_new ();

    engine->details->connection = tracker_sparql_connection_get (NULL, &error);

    if (error)
    {
        g_warning ("Could not establish a connection to Tracker: %s", error->message);
        g_error_free (error);
    }
}


NautilusSearchEngineTracker *
nautilus_search_engine_tracker_new (void)
{
    return g_object_new (NAUTILUS_TYPE_SEARCH_ENGINE_TRACKER, NULL);
}
