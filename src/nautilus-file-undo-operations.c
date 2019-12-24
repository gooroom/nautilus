/* nautilus-file-undo-operations.c - Manages undo/redo of file operations
 *
 * Copyright (C) 2007-2011 Amos Brocco
 * Copyright (C) 2010, 2012 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: Amos Brocco <amos.brocco@gmail.com>
 *          Cosimo Cecchi <cosimoc@redhat.com>
 *
 */

#include <config.h>
#include <stdlib.h>

#include "nautilus-file-undo-operations.h"

#include <glib/gi18n.h>

#include "nautilus-file-operations.h"
#include "nautilus-file.h"
#include "nautilus-file-undo-manager.h"
#include "nautilus-batch-rename-dialog.h"
#include "nautilus-batch-rename-utilities.h"


/* Since we use g_get_current_time for setting "orig_trash_time" in the undo
 * info, there are situations where the difference between this value and the
 * real deletion time can differ enough to make the rounding a difference of 1
 * second, failing the equality check. To make sure we avoid this, and to be
 * preventive, use 2 seconds epsilon.
 */
#define TRASH_TIME_EPSILON 2

G_DEFINE_TYPE (NautilusFileUndoInfo, nautilus_file_undo_info, G_TYPE_OBJECT)

enum
{
    PROP_OP_TYPE = 1,
    PROP_ITEM_COUNT,
    N_PROPERTIES
};

static GParamSpec *properties[N_PROPERTIES] = { NULL, };

struct _NautilusFileUndoInfoDetails
{
    NautilusFileUndoOp op_type;
    guint count;                /* Number of items */

    GTask *apply_async_task;

    gchar *undo_label;
    gchar *redo_label;
    gchar *undo_description;
    gchar *redo_description;
};

/* description helpers */
static void
nautilus_file_undo_info_init (NautilusFileUndoInfo *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, NAUTILUS_TYPE_FILE_UNDO_INFO,
                                              NautilusFileUndoInfoDetails);
    self->priv->apply_async_task = NULL;
}

static void
nautilus_file_undo_info_get_property (GObject    *object,
                                      guint       property_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
    NautilusFileUndoInfo *self = NAUTILUS_FILE_UNDO_INFO (object);

    switch (property_id)
    {
        case PROP_OP_TYPE:
        {
            g_value_set_int (value, self->priv->op_type);
        }
        break;

        case PROP_ITEM_COUNT:
        {
            g_value_set_int (value, self->priv->count);
        }
        break;

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        }
        break;
    }
}

static void
nautilus_file_undo_info_set_property (GObject      *object,
                                      guint         property_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
    NautilusFileUndoInfo *self = NAUTILUS_FILE_UNDO_INFO (object);

    switch (property_id)
    {
        case PROP_OP_TYPE:
        {
            self->priv->op_type = g_value_get_int (value);
        }
        break;

        case PROP_ITEM_COUNT:
        {
            self->priv->count = g_value_get_int (value);
        }
        break;

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        }
        break;
    }
}

static void
nautilus_file_redo_info_warn_redo (NautilusFileUndoInfo *self,
                                   GtkWindow            *parent_window)
{
    g_critical ("Object %p of type %s does not implement redo_func!!",
                self, G_OBJECT_TYPE_NAME (self));
}

static void
nautilus_file_undo_info_warn_undo (NautilusFileUndoInfo *self,
                                   GtkWindow            *parent_window)
{
    g_critical ("Object %p of type %s does not implement undo_func!!",
                self, G_OBJECT_TYPE_NAME (self));
}

static void
nautilus_file_undo_info_strings_func (NautilusFileUndoInfo  *self,
                                      gchar                **undo_label,
                                      gchar                **undo_description,
                                      gchar                **redo_label,
                                      gchar                **redo_description)
{
    if (undo_label != NULL)
    {
        *undo_label = g_strdup (_("Undo"));
    }
    if (undo_description != NULL)
    {
        *undo_description = g_strdup (_("Undo last action"));
    }

    if (redo_label != NULL)
    {
        *redo_label = g_strdup (_("Redo"));
    }
    if (redo_description != NULL)
    {
        *redo_description = g_strdup (_("Redo last undone action"));
    }
}

static void
nautilus_file_undo_info_finalize (GObject *obj)
{
    NautilusFileUndoInfo *self = NAUTILUS_FILE_UNDO_INFO (obj);

    g_clear_object (&self->priv->apply_async_task);

    G_OBJECT_CLASS (nautilus_file_undo_info_parent_class)->finalize (obj);
}

static void
nautilus_file_undo_info_class_init (NautilusFileUndoInfoClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS (klass);

    oclass->finalize = nautilus_file_undo_info_finalize;
    oclass->get_property = nautilus_file_undo_info_get_property;
    oclass->set_property = nautilus_file_undo_info_set_property;

    klass->undo_func = nautilus_file_undo_info_warn_undo;
    klass->redo_func = nautilus_file_redo_info_warn_redo;
    klass->strings_func = nautilus_file_undo_info_strings_func;

    properties[PROP_OP_TYPE] =
        g_param_spec_int ("op-type",
                          "Undo info op type",
                          "Type of undo operation",
                          0, NAUTILUS_FILE_UNDO_OP_NUM_TYPES - 1, 0,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY);
    properties[PROP_ITEM_COUNT] =
        g_param_spec_int ("item-count",
                          "Number of items",
                          "Number of items",
                          0, G_MAXINT, 0,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY);

    g_type_class_add_private (klass, sizeof (NautilusFileUndoInfoDetails));
    g_object_class_install_properties (oclass, N_PROPERTIES, properties);
}

NautilusFileUndoOp
nautilus_file_undo_info_get_op_type (NautilusFileUndoInfo *self)
{
    return self->priv->op_type;
}

static gint
nautilus_file_undo_info_get_item_count (NautilusFileUndoInfo *self)
{
    return self->priv->count;
}

void
nautilus_file_undo_info_apply_async (NautilusFileUndoInfo *self,
                                     gboolean              undo,
                                     GtkWindow            *parent_window,
                                     GAsyncReadyCallback   callback,
                                     gpointer              user_data)
{
    g_assert (self->priv->apply_async_task == NULL);

    self->priv->apply_async_task = g_task_new (G_OBJECT (self),
                                               NULL,
                                               callback,
                                               user_data);

    if (undo)
    {
        NAUTILUS_FILE_UNDO_INFO_CLASS (G_OBJECT_GET_CLASS (self))->undo_func (self, parent_window);
    }
    else
    {
        NAUTILUS_FILE_UNDO_INFO_CLASS (G_OBJECT_GET_CLASS (self))->redo_func (self, parent_window);
    }
}

typedef struct
{
    gboolean success;
    gboolean user_cancel;
} FileUndoInfoOpRes;

static void
file_undo_info_op_res_free (gpointer data)
{
    g_slice_free (FileUndoInfoOpRes, data);
}

gboolean
nautilus_file_undo_info_apply_finish (NautilusFileUndoInfo  *self,
                                      GAsyncResult          *res,
                                      gboolean              *user_cancel,
                                      GError               **error)
{
    FileUndoInfoOpRes *op_res;
    gboolean success = FALSE;

    op_res = g_task_propagate_pointer (G_TASK (res), error);

    if (op_res != NULL)
    {
        *user_cancel = op_res->user_cancel;
        success = op_res->success;

        file_undo_info_op_res_free (op_res);
    }

    return success;
}

void
nautilus_file_undo_info_get_strings (NautilusFileUndoInfo  *self,
                                     gchar                **undo_label,
                                     gchar                **undo_description,
                                     gchar                **redo_label,
                                     gchar                **redo_description)
{
    NAUTILUS_FILE_UNDO_INFO_CLASS (G_OBJECT_GET_CLASS (self))->strings_func (self,
                                                                             undo_label, undo_description,
                                                                             redo_label, redo_description);
}

static void
file_undo_info_complete_apply (NautilusFileUndoInfo *self,
                               gboolean              success,
                               gboolean              user_cancel)
{
    FileUndoInfoOpRes *op_res = g_slice_new0 (FileUndoInfoOpRes);

    op_res->user_cancel = user_cancel;
    op_res->success = success;

    g_task_return_pointer (self->priv->apply_async_task, op_res,
                           file_undo_info_op_res_free);

    g_clear_object (&self->priv->apply_async_task);
}

static void
file_undo_info_transfer_callback (GHashTable *debuting_uris,
                                  gboolean    success,
                                  gpointer    user_data)
{
    NautilusFileUndoInfo *self = user_data;

    /* TODO: we need to forward the cancelled state from
     * the file operation to the file undo info object.
     */
    file_undo_info_complete_apply (self, success, FALSE);
}

static void
file_undo_info_operation_callback (NautilusFile *file,
                                   GFile        *result_location,
                                   GError       *error,
                                   gpointer      user_data)
{
    NautilusFileUndoInfo *self = user_data;

    file_undo_info_complete_apply (self, (error == NULL),
                                   g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED));
}

static void
file_undo_info_delete_callback (GHashTable *debuting_uris,
                                gboolean    user_cancel,
                                gpointer    user_data)
{
    NautilusFileUndoInfo *self = user_data;

    file_undo_info_complete_apply (self,
                                   !user_cancel,
                                   user_cancel);
}

/* copy/move/duplicate/link/restore from trash */
G_DEFINE_TYPE (NautilusFileUndoInfoExt, nautilus_file_undo_info_ext, NAUTILUS_TYPE_FILE_UNDO_INFO)

struct _NautilusFileUndoInfoExtDetails
{
    GFile *src_dir;
    GFile *dest_dir;
    GQueue *sources;          /* Relative to src_dir */
    GQueue *destinations;     /* Relative to dest_dir */
};

static char *
ext_get_first_target_short_name (NautilusFileUndoInfoExt *self)
{
    GList *targets_first;
    char *file_name = NULL;

    targets_first = g_queue_peek_head_link (self->priv->destinations);

    if (targets_first != NULL &&
        targets_first->data != NULL)
    {
        file_name = g_file_get_basename (targets_first->data);
    }

    return file_name;
}

static void
ext_strings_func (NautilusFileUndoInfo  *info,
                  gchar                **undo_label,
                  gchar                **undo_description,
                  gchar                **redo_label,
                  gchar                **redo_description)
{
    NautilusFileUndoInfoExt *self = NAUTILUS_FILE_UNDO_INFO_EXT (info);
    NautilusFileUndoOp op_type = nautilus_file_undo_info_get_op_type (info);
    gint count = nautilus_file_undo_info_get_item_count (info);
    gchar *name = NULL, *source, *destination;

    source = g_file_get_path (self->priv->src_dir);
    destination = g_file_get_path (self->priv->dest_dir);

    if (count <= 1)
    {
        name = ext_get_first_target_short_name (self);
    }

    if (op_type == NAUTILUS_FILE_UNDO_OP_MOVE)
    {
        if (count > 1)
        {
            *undo_description = g_strdup_printf (ngettext ("Move %d item back to “%s”",
                                                           "Move %d items back to “%s”", count),
                                                 count, source);
            *redo_description = g_strdup_printf (ngettext ("Move %d item to “%s”",
                                                           "Move %d items to “%s”", count),
                                                 count, destination);

            *undo_label = g_strdup_printf (ngettext ("_Undo Move %d item",
                                                     "_Undo Move %d items", count),
                                           count);
            *redo_label = g_strdup_printf (ngettext ("_Redo Move %d item",
                                                     "_Redo Move %d items", count),
                                           count);
        }
        else
        {
            *undo_description = g_strdup_printf (_("Move “%s” back to “%s”"), name, source);
            *redo_description = g_strdup_printf (_("Move “%s” to “%s”"), name, destination);

            *undo_label = g_strdup (_("_Undo Move"));
            *redo_label = g_strdup (_("_Redo Move"));
        }
    }
    else if (op_type == NAUTILUS_FILE_UNDO_OP_RESTORE_FROM_TRASH)
    {
        *undo_label = g_strdup (_("_Undo Restore from Trash"));
        *redo_label = g_strdup (_("_Redo Restore from Trash"));

        if (count > 1)
        {
            *undo_description = g_strdup_printf (ngettext ("Move %d item back to trash",
                                                           "Move %d items back to trash", count),
                                                 count);
            *redo_description = g_strdup_printf (ngettext ("Restore %d item from trash",
                                                           "Restore %d items from trash", count),
                                                 count);
        }
        else
        {
            *undo_description = g_strdup_printf (_("Move “%s” back to trash"), name);
            *redo_description = g_strdup_printf (_("Restore “%s” from trash"), name);
        }
    }
    else if (op_type == NAUTILUS_FILE_UNDO_OP_COPY)
    {
        if (count > 1)
        {
            *undo_description = g_strdup_printf (ngettext ("Delete %d copied item",
                                                           "Delete %d copied items", count),
                                                 count);
            *redo_description = g_strdup_printf (ngettext ("Copy %d item to “%s”",
                                                           "Copy %d items to “%s”", count),
                                                 count, destination);

            *undo_label = g_strdup_printf (ngettext ("_Undo Copy %d item",
                                                     "_Undo Copy %d items", count),
                                           count);
            *redo_label = g_strdup_printf (ngettext ("_Redo Copy %d item",
                                                     "_Redo Copy %d items", count),
                                           count);
        }
        else
        {
            *undo_description = g_strdup_printf (_("Delete “%s”"), name);
            *redo_description = g_strdup_printf (_("Copy “%s” to “%s”"), name, destination);

            *undo_label = g_strdup (_("_Undo Copy"));
            *redo_label = g_strdup (_("_Redo Copy"));
        }
    }
    else if (op_type == NAUTILUS_FILE_UNDO_OP_DUPLICATE)
    {
        if (count > 1)
        {
            *undo_description = g_strdup_printf (ngettext ("Delete %d duplicated item",
                                                           "Delete %d duplicated items", count),
                                                 count);
            *redo_description = g_strdup_printf (ngettext ("Duplicate %d item in “%s”",
                                                           "Duplicate %d items in “%s”", count),
                                                 count, destination);

            *undo_label = g_strdup_printf (ngettext ("_Undo Duplicate %d item",
                                                     "_Undo Duplicate %d items", count),
                                           count);
            *redo_label = g_strdup_printf (ngettext ("_Redo Duplicate %d item",
                                                     "_Redo Duplicate %d items", count),
                                           count);
        }
        else
        {
            *undo_description = g_strdup_printf (_("Delete “%s”"), name);
            *redo_description = g_strdup_printf (_("Duplicate “%s” in “%s”"),
                                                 name, destination);

            *undo_label = g_strdup (_("_Undo Duplicate"));
            *redo_label = g_strdup (_("_Redo Duplicate"));
        }
    }
    else if (op_type == NAUTILUS_FILE_UNDO_OP_CREATE_LINK)
    {
        if (count > 1)
        {
            *undo_description = g_strdup_printf (ngettext ("Delete links to %d item",
                                                           "Delete links to %d items", count),
                                                 count);
            *redo_description = g_strdup_printf (ngettext ("Create links to %d item",
                                                           "Create links to %d items", count),
                                                 count);
        }
        else
        {
            *undo_description = g_strdup_printf (_("Delete link to “%s”"), name);
            *redo_description = g_strdup_printf (_("Create link to “%s”"), name);

            *undo_label = g_strdup (_("_Undo Create Link"));
            *redo_label = g_strdup (_("_Redo Create Link"));
        }
    }
    else
    {
        g_assert_not_reached ();
    }

    g_free (name);
    g_free (source);
    g_free (destination);
}

static void
ext_create_link_redo_func (NautilusFileUndoInfoExt *self,
                           GtkWindow               *parent_window)
{
    nautilus_file_operations_link (g_queue_peek_head_link (self->priv->sources),
                                   NULL,
                                   self->priv->dest_dir,
                                   parent_window,
                                   file_undo_info_transfer_callback,
                                   self);
}

static void
ext_duplicate_redo_func (NautilusFileUndoInfoExt *self,
                         GtkWindow               *parent_window)
{
    nautilus_file_operations_duplicate (g_queue_peek_head_link (self->priv->sources),
                                        NULL,
                                        parent_window,
                                        file_undo_info_transfer_callback,
                                        self);
}

static void
ext_copy_redo_func (NautilusFileUndoInfoExt *self,
                    GtkWindow               *parent_window)
{
    nautilus_file_operations_copy (g_queue_peek_head_link (self->priv->sources),
                                   NULL,
                                   self->priv->dest_dir,
                                   parent_window,
                                   file_undo_info_transfer_callback,
                                   self);
}

static void
ext_move_restore_redo_func (NautilusFileUndoInfoExt *self,
                            GtkWindow               *parent_window)
{
    nautilus_file_operations_move (g_queue_peek_head_link (self->priv->sources),
                                   NULL,
                                   self->priv->dest_dir,
                                   parent_window,
                                   file_undo_info_transfer_callback,
                                   self);
}

static void
ext_redo_func (NautilusFileUndoInfo *info,
               GtkWindow            *parent_window)
{
    NautilusFileUndoInfoExt *self = NAUTILUS_FILE_UNDO_INFO_EXT (info);
    NautilusFileUndoOp op_type = nautilus_file_undo_info_get_op_type (info);

    if (op_type == NAUTILUS_FILE_UNDO_OP_MOVE ||
        op_type == NAUTILUS_FILE_UNDO_OP_RESTORE_FROM_TRASH)
    {
        ext_move_restore_redo_func (self, parent_window);
    }
    else if (op_type == NAUTILUS_FILE_UNDO_OP_COPY)
    {
        ext_copy_redo_func (self, parent_window);
    }
    else if (op_type == NAUTILUS_FILE_UNDO_OP_DUPLICATE)
    {
        ext_duplicate_redo_func (self, parent_window);
    }
    else if (op_type == NAUTILUS_FILE_UNDO_OP_CREATE_LINK)
    {
        ext_create_link_redo_func (self, parent_window);
    }
    else
    {
        g_assert_not_reached ();
    }
}

static void
ext_restore_undo_func (NautilusFileUndoInfoExt *self,
                       GtkWindow               *parent_window)
{
    nautilus_file_operations_trash_or_delete (g_queue_peek_head_link (self->priv->destinations),
                                              parent_window,
                                              file_undo_info_delete_callback,
                                              self);
}


static void
ext_move_undo_func (NautilusFileUndoInfoExt *self,
                    GtkWindow               *parent_window)
{
    nautilus_file_operations_move (g_queue_peek_head_link (self->priv->destinations),
                                   NULL,
                                   self->priv->src_dir,
                                   parent_window,
                                   file_undo_info_transfer_callback,
                                   self);
}

static void
ext_copy_duplicate_undo_func (NautilusFileUndoInfoExt *self,
                              GtkWindow               *parent_window)
{
    GList *files;

    files = g_list_copy (g_queue_peek_head_link (self->priv->destinations));
    files = g_list_reverse (files);     /* Deleting must be done in reverse */

    nautilus_file_operations_delete (files, parent_window,
                                     file_undo_info_delete_callback, self);

    g_list_free (files);
}

static void
ext_undo_func (NautilusFileUndoInfo *info,
               GtkWindow            *parent_window)
{
    NautilusFileUndoInfoExt *self = NAUTILUS_FILE_UNDO_INFO_EXT (info);
    NautilusFileUndoOp op_type = nautilus_file_undo_info_get_op_type (info);

    if (op_type == NAUTILUS_FILE_UNDO_OP_COPY ||
        op_type == NAUTILUS_FILE_UNDO_OP_DUPLICATE ||
        op_type == NAUTILUS_FILE_UNDO_OP_CREATE_LINK)
    {
        ext_copy_duplicate_undo_func (self, parent_window);
    }
    else if (op_type == NAUTILUS_FILE_UNDO_OP_MOVE)
    {
        ext_move_undo_func (self, parent_window);
    }
    else if (op_type == NAUTILUS_FILE_UNDO_OP_RESTORE_FROM_TRASH)
    {
        ext_restore_undo_func (self, parent_window);
    }
    else
    {
        g_assert_not_reached ();
    }
}

static void
nautilus_file_undo_info_ext_init (NautilusFileUndoInfoExt *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, nautilus_file_undo_info_ext_get_type (),
                                              NautilusFileUndoInfoExtDetails);
}

static void
nautilus_file_undo_info_ext_finalize (GObject *obj)
{
    NautilusFileUndoInfoExt *self = NAUTILUS_FILE_UNDO_INFO_EXT (obj);

    if (self->priv->sources)
    {
        g_queue_free_full (self->priv->sources, g_object_unref);
    }

    if (self->priv->destinations)
    {
        g_queue_free_full (self->priv->destinations, g_object_unref);
    }

    g_clear_object (&self->priv->src_dir);
    g_clear_object (&self->priv->dest_dir);

    G_OBJECT_CLASS (nautilus_file_undo_info_ext_parent_class)->finalize (obj);
}

static void
nautilus_file_undo_info_ext_class_init (NautilusFileUndoInfoExtClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS (klass);
    NautilusFileUndoInfoClass *iclass = NAUTILUS_FILE_UNDO_INFO_CLASS (klass);

    oclass->finalize = nautilus_file_undo_info_ext_finalize;

    iclass->undo_func = ext_undo_func;
    iclass->redo_func = ext_redo_func;
    iclass->strings_func = ext_strings_func;

    g_type_class_add_private (klass, sizeof (NautilusFileUndoInfoExtDetails));
}

NautilusFileUndoInfo *
nautilus_file_undo_info_ext_new (NautilusFileUndoOp  op_type,
                                 gint                item_count,
                                 GFile              *src_dir,
                                 GFile              *target_dir)
{
    NautilusFileUndoInfoExt *retval;

    retval = g_object_new (NAUTILUS_TYPE_FILE_UNDO_INFO_EXT,
                           "op-type", op_type,
                           "item-count", item_count,
                           NULL);

    retval->priv->src_dir = g_object_ref (src_dir);
    retval->priv->dest_dir = g_object_ref (target_dir);
    retval->priv->sources = g_queue_new ();
    retval->priv->destinations = g_queue_new ();

    return NAUTILUS_FILE_UNDO_INFO (retval);
}

void
nautilus_file_undo_info_ext_add_origin_target_pair (NautilusFileUndoInfoExt *self,
                                                    GFile                   *origin,
                                                    GFile                   *target)
{
    g_queue_push_tail (self->priv->sources, g_object_ref (origin));
    g_queue_push_tail (self->priv->destinations, g_object_ref (target));
}

/* create new file/folder */
G_DEFINE_TYPE (NautilusFileUndoInfoCreate, nautilus_file_undo_info_create, NAUTILUS_TYPE_FILE_UNDO_INFO)

struct _NautilusFileUndoInfoCreateDetails
{
    char *template;
    GFile *target_file;
    gint length;
};

static void
create_strings_func (NautilusFileUndoInfo  *info,
                     gchar                **undo_label,
                     gchar                **undo_description,
                     gchar                **redo_label,
                     gchar                **redo_description)
{
    NautilusFileUndoInfoCreate *self = NAUTILUS_FILE_UNDO_INFO_CREATE (info);
    NautilusFileUndoOp op_type = nautilus_file_undo_info_get_op_type (info);
    char *name;

    name = g_file_get_parse_name (self->priv->target_file);
    *undo_description = g_strdup_printf (_("Delete “%s”"), name);

    if (op_type == NAUTILUS_FILE_UNDO_OP_CREATE_EMPTY_FILE)
    {
        *redo_description = g_strdup_printf (_("Create an empty file “%s”"), name);

        *undo_label = g_strdup (_("_Undo Create Empty File"));
        *redo_label = g_strdup (_("_Redo Create Empty File"));
    }
    else if (op_type == NAUTILUS_FILE_UNDO_OP_CREATE_FOLDER)
    {
        *redo_description = g_strdup_printf (_("Create a new folder “%s”"), name);

        *undo_label = g_strdup (_("_Undo Create Folder"));
        *redo_label = g_strdup (_("_Redo Create Folder"));
    }
    else if (op_type == NAUTILUS_FILE_UNDO_OP_CREATE_FILE_FROM_TEMPLATE)
    {
        *redo_description = g_strdup_printf (_("Create new file “%s” from template "), name);

        *undo_label = g_strdup (_("_Undo Create from Template"));
        *redo_label = g_strdup (_("_Redo Create from Template"));
    }
    else
    {
        g_assert_not_reached ();
    }

    g_free (name);
}

static void
create_callback (GFile    *new_file,
                 gboolean  success,
                 gpointer  callback_data)
{
    file_undo_info_transfer_callback (NULL, success, callback_data);
}

static void
create_from_template_redo_func (NautilusFileUndoInfoCreate *self,
                                GtkWindow                  *parent_window)
{
    GFile *parent;
    gchar *parent_uri, *new_name;

    parent = g_file_get_parent (self->priv->target_file);
    parent_uri = g_file_get_uri (parent);
    new_name = g_file_get_parse_name (self->priv->target_file);
    nautilus_file_operations_new_file_from_template (NULL, NULL,
                                                     parent_uri, new_name,
                                                     self->priv->template,
                                                     create_callback, self);

    g_free (parent_uri);
    g_free (new_name);
    g_object_unref (parent);
}

static void
create_folder_redo_func (NautilusFileUndoInfoCreate *self,
                         GtkWindow                  *parent_window)
{
    GFile *parent;
    gchar *parent_uri;
    gchar *name;

    name = g_file_get_basename (self->priv->target_file);
    parent = g_file_get_parent (self->priv->target_file);
    parent_uri = g_file_get_uri (parent);
    nautilus_file_operations_new_folder (NULL, NULL, parent_uri, name,
                                         create_callback, self);

    g_free (name);
    g_free (parent_uri);
    g_object_unref (parent);
}

static void
create_empty_redo_func (NautilusFileUndoInfoCreate *self,
                        GtkWindow                  *parent_window)
{
    GFile *parent;
    gchar *parent_uri;
    gchar *new_name;

    parent = g_file_get_parent (self->priv->target_file);
    parent_uri = g_file_get_uri (parent);
    new_name = g_file_get_parse_name (self->priv->target_file);
    nautilus_file_operations_new_file (NULL, NULL, parent_uri,
                                       new_name,
                                       self->priv->template,
                                       self->priv->length,
                                       create_callback, self);

    g_free (parent_uri);
    g_free (new_name);
    g_object_unref (parent);
}

static void
create_redo_func (NautilusFileUndoInfo *info,
                  GtkWindow            *parent_window)
{
    NautilusFileUndoInfoCreate *self = NAUTILUS_FILE_UNDO_INFO_CREATE (info);
    NautilusFileUndoOp op_type = nautilus_file_undo_info_get_op_type (info);

    if (op_type == NAUTILUS_FILE_UNDO_OP_CREATE_EMPTY_FILE)
    {
        create_empty_redo_func (self, parent_window);
    }
    else if (op_type == NAUTILUS_FILE_UNDO_OP_CREATE_FOLDER)
    {
        create_folder_redo_func (self, parent_window);
    }
    else if (op_type == NAUTILUS_FILE_UNDO_OP_CREATE_FILE_FROM_TEMPLATE)
    {
        create_from_template_redo_func (self, parent_window);
    }
    else
    {
        g_assert_not_reached ();
    }
}

static void
create_undo_func (NautilusFileUndoInfo *info,
                  GtkWindow            *parent_window)
{
    NautilusFileUndoInfoCreate *self = NAUTILUS_FILE_UNDO_INFO_CREATE (info);
    GList *files = NULL;

    files = g_list_append (files, g_object_ref (self->priv->target_file));
    nautilus_file_operations_delete (files, parent_window,
                                     file_undo_info_delete_callback, self);

    g_list_free_full (files, g_object_unref);
}

static void
nautilus_file_undo_info_create_init (NautilusFileUndoInfoCreate *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, nautilus_file_undo_info_create_get_type (),
                                              NautilusFileUndoInfoCreateDetails);
}

static void
nautilus_file_undo_info_create_finalize (GObject *obj)
{
    NautilusFileUndoInfoCreate *self = NAUTILUS_FILE_UNDO_INFO_CREATE (obj);
    g_clear_object (&self->priv->target_file);
    g_free (self->priv->template);

    G_OBJECT_CLASS (nautilus_file_undo_info_create_parent_class)->finalize (obj);
}

static void
nautilus_file_undo_info_create_class_init (NautilusFileUndoInfoCreateClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS (klass);
    NautilusFileUndoInfoClass *iclass = NAUTILUS_FILE_UNDO_INFO_CLASS (klass);

    oclass->finalize = nautilus_file_undo_info_create_finalize;

    iclass->undo_func = create_undo_func;
    iclass->redo_func = create_redo_func;
    iclass->strings_func = create_strings_func;

    g_type_class_add_private (klass, sizeof (NautilusFileUndoInfoCreateDetails));
}

NautilusFileUndoInfo *
nautilus_file_undo_info_create_new (NautilusFileUndoOp op_type)
{
    return g_object_new (NAUTILUS_TYPE_FILE_UNDO_INFO_CREATE,
                         "op-type", op_type,
                         "item-count", 1,
                         NULL);
}

void
nautilus_file_undo_info_create_set_data (NautilusFileUndoInfoCreate *self,
                                         GFile                      *file,
                                         const char                 *template,
                                         gint                        length)
{
    self->priv->target_file = g_object_ref (file);
    self->priv->template = g_strdup (template);
    self->priv->length = length;
}

/* rename */
G_DEFINE_TYPE (NautilusFileUndoInfoRename, nautilus_file_undo_info_rename, NAUTILUS_TYPE_FILE_UNDO_INFO)

struct _NautilusFileUndoInfoRenameDetails
{
    GFile *old_file;
    GFile *new_file;
    gchar *old_display_name;
    gchar *new_display_name;
};

static void
rename_strings_func (NautilusFileUndoInfo  *info,
                     gchar                **undo_label,
                     gchar                **undo_description,
                     gchar                **redo_label,
                     gchar                **redo_description)
{
    NautilusFileUndoInfoRename *self = NAUTILUS_FILE_UNDO_INFO_RENAME (info);
    gchar *new_name, *old_name;

    new_name = g_file_get_parse_name (self->priv->new_file);
    old_name = g_file_get_parse_name (self->priv->old_file);

    *undo_description = g_strdup_printf (_("Rename “%s” as “%s”"), new_name, old_name);
    *redo_description = g_strdup_printf (_("Rename “%s” as “%s”"), old_name, new_name);

    *undo_label = g_strdup (_("_Undo Rename"));
    *redo_label = g_strdup (_("_Redo Rename"));

    g_free (old_name);
    g_free (new_name);
}

static void
rename_redo_func (NautilusFileUndoInfo *info,
                  GtkWindow            *parent_window)
{
    NautilusFileUndoInfoRename *self = NAUTILUS_FILE_UNDO_INFO_RENAME (info);
    NautilusFile *file;

    file = nautilus_file_get (self->priv->old_file);
    nautilus_file_rename (file, self->priv->new_display_name,
                          file_undo_info_operation_callback, self);

    nautilus_file_unref (file);
}

static void
rename_undo_func (NautilusFileUndoInfo *info,
                  GtkWindow            *parent_window)
{
    NautilusFileUndoInfoRename *self = NAUTILUS_FILE_UNDO_INFO_RENAME (info);
    NautilusFile *file;

    file = nautilus_file_get (self->priv->new_file);
    nautilus_file_rename (file, self->priv->old_display_name,
                          file_undo_info_operation_callback, self);

    nautilus_file_unref (file);
}

static void
nautilus_file_undo_info_rename_init (NautilusFileUndoInfoRename *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, nautilus_file_undo_info_rename_get_type (),
                                              NautilusFileUndoInfoRenameDetails);
}

static void
nautilus_file_undo_info_rename_finalize (GObject *obj)
{
    NautilusFileUndoInfoRename *self = NAUTILUS_FILE_UNDO_INFO_RENAME (obj);
    g_clear_object (&self->priv->old_file);
    g_clear_object (&self->priv->new_file);
    g_free (self->priv->old_display_name);
    g_free (self->priv->new_display_name);

    G_OBJECT_CLASS (nautilus_file_undo_info_rename_parent_class)->finalize (obj);
}

static void
nautilus_file_undo_info_rename_class_init (NautilusFileUndoInfoRenameClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS (klass);
    NautilusFileUndoInfoClass *iclass = NAUTILUS_FILE_UNDO_INFO_CLASS (klass);

    oclass->finalize = nautilus_file_undo_info_rename_finalize;

    iclass->undo_func = rename_undo_func;
    iclass->redo_func = rename_redo_func;
    iclass->strings_func = rename_strings_func;

    g_type_class_add_private (klass, sizeof (NautilusFileUndoInfoRenameDetails));
}

NautilusFileUndoInfo *
nautilus_file_undo_info_rename_new (void)
{
    return g_object_new (NAUTILUS_TYPE_FILE_UNDO_INFO_RENAME,
                         "op-type", NAUTILUS_FILE_UNDO_OP_RENAME,
                         "item-count", 1,
                         NULL);
}

void
nautilus_file_undo_info_rename_set_data_pre (NautilusFileUndoInfoRename *self,
                                             GFile                      *old_file,
                                             gchar                      *old_display_name,
                                             gchar                      *new_display_name)
{
    self->priv->old_file = g_object_ref (old_file);
    self->priv->old_display_name = g_strdup (old_display_name);
    self->priv->new_display_name = g_strdup (new_display_name);
}

void
nautilus_file_undo_info_rename_set_data_post (NautilusFileUndoInfoRename *self,
                                              GFile                      *new_file)
{
    self->priv->new_file = g_object_ref (new_file);
}

/* batch rename */
G_DEFINE_TYPE (NautilusFileUndoInfoBatchRename, nautilus_file_undo_info_batch_rename, NAUTILUS_TYPE_FILE_UNDO_INFO);

struct _NautilusFileUndoInfoBatchRenameDetails
{
    GList *old_files;
    GList *new_files;
    GList *old_display_names;
    GList *new_display_names;
};

static void
batch_rename_strings_func (NautilusFileUndoInfo  *info,
                           gchar                **undo_label,
                           gchar                **undo_description,
                           gchar                **redo_label,
                           gchar                **redo_description)
{
    NautilusFileUndoInfoBatchRename *self = NAUTILUS_FILE_UNDO_INFO_BATCH_RENAME (info);

    *undo_description = g_strdup_printf (ngettext ("Batch rename %d file",
                                                   "Batch rename %d files",
                                                   g_list_length (self->priv->new_files)),
                                         g_list_length (self->priv->new_files));
    *redo_description = g_strdup_printf (ngettext ("Batch rename %d file",
                                                   "Batch rename %d files",
                                                   g_list_length (self->priv->new_files)),
                                         g_list_length (self->priv->new_files));

    *undo_label = g_strdup (_("_Undo Batch Rename"));
    *redo_label = g_strdup (_("_Redo Batch Rename"));
}

static void
batch_rename_redo_func (NautilusFileUndoInfo *info,
                        GtkWindow            *parent_window)
{
    NautilusFileUndoInfoBatchRename *self = NAUTILUS_FILE_UNDO_INFO_BATCH_RENAME (info);

    GList *l, *files;
    NautilusFile *file;
    GFile *old_file;

    files = NULL;

    for (l = self->priv->old_files; l != NULL; l = l->next)
    {
        old_file = l->data;

        file = nautilus_file_get (old_file);
        files = g_list_prepend (files, file);
    }

    files = g_list_reverse (files);

    batch_rename_sort_lists_for_rename (&files,
                                        &self->priv->new_display_names,
                                        &self->priv->old_display_names,
                                        &self->priv->new_files,
                                        &self->priv->old_files,
                                        TRUE);

    nautilus_file_batch_rename (files, self->priv->new_display_names, file_undo_info_operation_callback, self);
}

static void
batch_rename_undo_func (NautilusFileUndoInfo *info,
                        GtkWindow            *parent_window)
{
    NautilusFileUndoInfoBatchRename *self = NAUTILUS_FILE_UNDO_INFO_BATCH_RENAME (info);

    GList *l, *files;
    NautilusFile *file;
    GFile *new_file;

    files = NULL;

    for (l = self->priv->new_files; l != NULL; l = l->next)
    {
        new_file = l->data;

        file = nautilus_file_get (new_file);
        files = g_list_prepend (files, file);
    }

    files = g_list_reverse (files);

    batch_rename_sort_lists_for_rename (&files,
                                        &self->priv->old_display_names,
                                        &self->priv->new_display_names,
                                        &self->priv->old_files,
                                        &self->priv->new_files,
                                        TRUE);

    nautilus_file_batch_rename (files, self->priv->old_display_names, file_undo_info_operation_callback, self);
}

static void
nautilus_file_undo_info_batch_rename_init (NautilusFileUndoInfoBatchRename *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, nautilus_file_undo_info_batch_rename_get_type (),
                                              NautilusFileUndoInfoBatchRenameDetails);
}

static void
nautilus_file_undo_info_batch_rename_finalize (GObject *obj)
{
    GList *l;
    GFile *file;
    GString *string;
    NautilusFileUndoInfoBatchRename *self = NAUTILUS_FILE_UNDO_INFO_BATCH_RENAME (obj);

    for (l = self->priv->new_files; l != NULL; l = l->next)
    {
        file = l->data;

        g_clear_object (&file);
    }

    for (l = self->priv->old_files; l != NULL; l = l->next)
    {
        file = l->data;

        g_clear_object (&file);
    }

    for (l = self->priv->new_display_names; l != NULL; l = l->next)
    {
        string = l->data;

        g_string_free (string, TRUE);
    }

    for (l = self->priv->old_display_names; l != NULL; l = l->next)
    {
        string = l->data;

        g_string_free (string, TRUE);
    }

    g_list_free (self->priv->new_files);
    g_list_free (self->priv->old_files);
    g_list_free (self->priv->new_display_names);
    g_list_free (self->priv->old_display_names);

    G_OBJECT_CLASS (nautilus_file_undo_info_batch_rename_parent_class)->finalize (obj);
}

static void
nautilus_file_undo_info_batch_rename_class_init (NautilusFileUndoInfoBatchRenameClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS (klass);
    NautilusFileUndoInfoClass *iclass = NAUTILUS_FILE_UNDO_INFO_CLASS (klass);

    oclass->finalize = nautilus_file_undo_info_batch_rename_finalize;

    iclass->undo_func = batch_rename_undo_func;
    iclass->redo_func = batch_rename_redo_func;
    iclass->strings_func = batch_rename_strings_func;

    g_type_class_add_private (klass, sizeof (NautilusFileUndoInfoBatchRenameDetails));
}

NautilusFileUndoInfo *
nautilus_file_undo_info_batch_rename_new (gint item_count)
{
    return g_object_new (NAUTILUS_TYPE_FILE_UNDO_INFO_BATCH_RENAME,
                         "op-type", NAUTILUS_FILE_UNDO_OP_BATCH_RENAME,
                         "item-count", item_count,
                         NULL);
}

void
nautilus_file_undo_info_batch_rename_set_data_pre (NautilusFileUndoInfoBatchRename *self,
                                                   GList                           *old_files)
{
    GList *l;
    GString *old_name;
    GFile *file;

    self->priv->old_files = old_files;
    self->priv->old_display_names = NULL;

    for (l = old_files; l != NULL; l = l->next)
    {
        file = l->data;

        old_name = g_string_new (g_file_get_basename (file));

        self->priv->old_display_names = g_list_prepend (self->priv->old_display_names, old_name);
    }

    self->priv->old_display_names = g_list_reverse (self->priv->old_display_names);
}

void
nautilus_file_undo_info_batch_rename_set_data_post (NautilusFileUndoInfoBatchRename *self,
                                                    GList                           *new_files)
{
    GList *l;
    GString *new_name;
    GFile *file;

    self->priv->new_files = new_files;
    self->priv->new_display_names = NULL;

    for (l = new_files; l != NULL; l = l->next)
    {
        file = l->data;

        new_name = g_string_new (g_file_get_basename (file));

        self->priv->new_display_names = g_list_prepend (self->priv->new_display_names, new_name);
    }

    self->priv->new_display_names = g_list_reverse (self->priv->new_display_names);
}

/* trash */
G_DEFINE_TYPE (NautilusFileUndoInfoTrash, nautilus_file_undo_info_trash, NAUTILUS_TYPE_FILE_UNDO_INFO)

struct _NautilusFileUndoInfoTrashDetails
{
    GHashTable *trashed;
};

static void
trash_strings_func (NautilusFileUndoInfo  *info,
                    gchar                **undo_label,
                    gchar                **undo_description,
                    gchar                **redo_label,
                    gchar                **redo_description)
{
    NautilusFileUndoInfoTrash *self = NAUTILUS_FILE_UNDO_INFO_TRASH (info);
    gint count = g_hash_table_size (self->priv->trashed);

    if (count != 1)
    {
        *undo_description = g_strdup_printf (ngettext ("Restore %d item from trash",
                                                       "Restore %d items from trash", count),
                                             count);
        *redo_description = g_strdup_printf (ngettext ("Move %d item to trash",
                                                       "Move %d items to trash", count),
                                             count);
    }
    else
    {
        GList *keys;
        char *name, *orig_path;
        GFile *file;

        keys = g_hash_table_get_keys (self->priv->trashed);
        file = keys->data;
        name = g_file_get_basename (file);
        orig_path = g_file_get_path (file);
        *undo_description = g_strdup_printf (_("Restore “%s” to “%s”"), name, orig_path);

        g_free (name);
        g_free (orig_path);
        g_list_free (keys);

        name = g_file_get_parse_name (file);
        *redo_description = g_strdup_printf (_("Move “%s” to trash"), name);

        g_free (name);
    }

    *undo_label = g_strdup (_("_Undo Trash"));
    *redo_label = g_strdup (_("_Redo Trash"));
}

static void
trash_redo_func_callback (GHashTable *debuting_uris,
                          gboolean    user_cancel,
                          gpointer    user_data)
{
    NautilusFileUndoInfoTrash *self = user_data;
    GHashTable *new_trashed_files;
    GTimeVal current_time;
    gsize updated_trash_time;
    GFile *file;
    GList *keys, *l;

    if (!user_cancel)
    {
        new_trashed_files =
            g_hash_table_new_full (g_file_hash, (GEqualFunc) g_file_equal,
                                   g_object_unref, NULL);

        keys = g_hash_table_get_keys (self->priv->trashed);

        g_get_current_time (&current_time);
        updated_trash_time = current_time.tv_sec;

        for (l = keys; l != NULL; l = l->next)
        {
            file = l->data;
            g_hash_table_insert (new_trashed_files,
                                 g_object_ref (file), GSIZE_TO_POINTER (updated_trash_time));
        }

        g_list_free (keys);
        g_hash_table_destroy (self->priv->trashed);

        self->priv->trashed = new_trashed_files;
    }

    file_undo_info_delete_callback (debuting_uris, user_cancel, user_data);
}

static void
trash_redo_func (NautilusFileUndoInfo *info,
                 GtkWindow            *parent_window)
{
    NautilusFileUndoInfoTrash *self = NAUTILUS_FILE_UNDO_INFO_TRASH (info);

    if (g_hash_table_size (self->priv->trashed) > 0)
    {
        GList *locations;

        locations = g_hash_table_get_keys (self->priv->trashed);
        nautilus_file_operations_trash_or_delete (locations, parent_window,
                                                  trash_redo_func_callback, self);

        g_list_free (locations);
    }
}

static void
trash_retrieve_files_to_restore_thread (GTask        *task,
                                        gpointer      source_object,
                                        gpointer      task_data,
                                        GCancellable *cancellable)
{
    NautilusFileUndoInfoTrash *self = NAUTILUS_FILE_UNDO_INFO_TRASH (source_object);
    GFileEnumerator *enumerator;
    GHashTable *to_restore;
    GFile *trash;
    GError *error = NULL;

    to_restore = g_hash_table_new_full (g_file_hash, (GEqualFunc) g_file_equal,
                                        g_object_unref, g_object_unref);

    trash = g_file_new_for_uri ("trash:///");

    enumerator = g_file_enumerate_children (trash,
                                            G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                            G_FILE_ATTRIBUTE_TRASH_DELETION_DATE ","
                                            G_FILE_ATTRIBUTE_TRASH_ORIG_PATH,
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            NULL, &error);

    if (enumerator)
    {
        GFileInfo *info;
        gpointer lookupvalue;
        GFile *item;
        glong trash_time, orig_trash_time;
        const char *origpath;
        GFile *origfile;

        while ((info = g_file_enumerator_next_file (enumerator, NULL, &error)) != NULL)
        {
            /* Retrieve the original file uri */
            origpath = g_file_info_get_attribute_byte_string (info, G_FILE_ATTRIBUTE_TRASH_ORIG_PATH);
            origfile = g_file_new_for_path (origpath);

            lookupvalue = g_hash_table_lookup (self->priv->trashed, origfile);

            if (lookupvalue)
            {
                GDateTime *date;

                orig_trash_time = GPOINTER_TO_SIZE (lookupvalue);
                trash_time = 0;
                date = g_file_info_get_deletion_date (info);
                if (date)
                {
                    trash_time = g_date_time_to_unix (date);
                    g_date_time_unref (date);
                }

                if (ABS (orig_trash_time - trash_time) <= TRASH_TIME_EPSILON)
                {
                    /* File in the trash */
                    item = g_file_get_child (trash, g_file_info_get_name (info));
                    g_hash_table_insert (to_restore, item, g_object_ref (origfile));
                }
            }

            g_object_unref (origfile);
        }
        g_file_enumerator_close (enumerator, FALSE, NULL);
        g_object_unref (enumerator);
    }
    g_object_unref (trash);

    if (error != NULL)
    {
        g_task_return_error (task, error);
        g_hash_table_destroy (to_restore);
    }
    else
    {
        g_task_return_pointer (task, to_restore, NULL);
    }
}

static void
trash_retrieve_files_to_restore_async (NautilusFileUndoInfoTrash *self,
                                       GAsyncReadyCallback        callback,
                                       gpointer                   user_data)
{
    GTask *task;

    task = g_task_new (G_OBJECT (self), NULL, callback, user_data);

    g_task_run_in_thread (task, trash_retrieve_files_to_restore_thread);

    g_object_unref (task);
}

static void
trash_retrieve_files_ready (GObject      *source,
                            GAsyncResult *res,
                            gpointer      user_data)
{
    NautilusFileUndoInfoTrash *self = NAUTILUS_FILE_UNDO_INFO_TRASH (source);
    GHashTable *files_to_restore;
    GError *error = NULL;

    files_to_restore = g_task_propagate_pointer (G_TASK (res), &error);

    if (error == NULL && g_hash_table_size (files_to_restore) > 0)
    {
        GList *gfiles_in_trash, *l;
        GFile *item;
        GFile *dest;

        gfiles_in_trash = g_hash_table_get_keys (files_to_restore);

        for (l = gfiles_in_trash; l != NULL; l = l->next)
        {
            item = l->data;
            dest = g_hash_table_lookup (files_to_restore, item);

            g_file_move (item, dest, G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL, NULL, NULL);
        }

        g_list_free (gfiles_in_trash);

        /* Here we must do what's necessary for the callback */
        file_undo_info_transfer_callback (NULL, (error == NULL), self);
    }
    else
    {
        file_undo_info_transfer_callback (NULL, FALSE, self);
    }

    if (files_to_restore != NULL)
    {
        g_hash_table_destroy (files_to_restore);
    }

    g_clear_error (&error);
}

static void
trash_undo_func (NautilusFileUndoInfo *info,
                 GtkWindow            *parent_window)
{
    NautilusFileUndoInfoTrash *self = NAUTILUS_FILE_UNDO_INFO_TRASH (info);

    trash_retrieve_files_to_restore_async (self, trash_retrieve_files_ready, NULL);
}

static void
nautilus_file_undo_info_trash_init (NautilusFileUndoInfoTrash *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, nautilus_file_undo_info_trash_get_type (),
                                              NautilusFileUndoInfoTrashDetails);
    self->priv->trashed =
        g_hash_table_new_full (g_file_hash, (GEqualFunc) g_file_equal,
                               g_object_unref, NULL);
}

static void
nautilus_file_undo_info_trash_finalize (GObject *obj)
{
    NautilusFileUndoInfoTrash *self = NAUTILUS_FILE_UNDO_INFO_TRASH (obj);
    g_hash_table_destroy (self->priv->trashed);

    G_OBJECT_CLASS (nautilus_file_undo_info_trash_parent_class)->finalize (obj);
}

static void
nautilus_file_undo_info_trash_class_init (NautilusFileUndoInfoTrashClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS (klass);
    NautilusFileUndoInfoClass *iclass = NAUTILUS_FILE_UNDO_INFO_CLASS (klass);

    oclass->finalize = nautilus_file_undo_info_trash_finalize;

    iclass->undo_func = trash_undo_func;
    iclass->redo_func = trash_redo_func;
    iclass->strings_func = trash_strings_func;

    g_type_class_add_private (klass, sizeof (NautilusFileUndoInfoTrashDetails));
}

NautilusFileUndoInfo *
nautilus_file_undo_info_trash_new (gint item_count)
{
    return g_object_new (NAUTILUS_TYPE_FILE_UNDO_INFO_TRASH,
                         "op-type", NAUTILUS_FILE_UNDO_OP_MOVE_TO_TRASH,
                         "item-count", item_count,
                         NULL);
}

void
nautilus_file_undo_info_trash_add_file (NautilusFileUndoInfoTrash *self,
                                        GFile                     *file)
{
    GTimeVal current_time;
    gsize orig_trash_time;

    g_get_current_time (&current_time);
    orig_trash_time = current_time.tv_sec;

    g_hash_table_insert (self->priv->trashed, g_object_ref (file), GSIZE_TO_POINTER (orig_trash_time));
}

GList *
nautilus_file_undo_info_trash_get_files (NautilusFileUndoInfoTrash *self)
{
    return g_hash_table_get_keys (self->priv->trashed);
}

/* recursive permissions */
G_DEFINE_TYPE (NautilusFileUndoInfoRecPermissions, nautilus_file_undo_info_rec_permissions, NAUTILUS_TYPE_FILE_UNDO_INFO)

struct _NautilusFileUndoInfoRecPermissionsDetails
{
    GFile *dest_dir;
    GHashTable *original_permissions;
    guint32 dir_mask;
    guint32 dir_permissions;
    guint32 file_mask;
    guint32 file_permissions;
};

static void
rec_permissions_strings_func (NautilusFileUndoInfo  *info,
                              gchar                **undo_label,
                              gchar                **undo_description,
                              gchar                **redo_label,
                              gchar                **redo_description)
{
    NautilusFileUndoInfoRecPermissions *self = NAUTILUS_FILE_UNDO_INFO_REC_PERMISSIONS (info);
    char *name;

    name = g_file_get_path (self->priv->dest_dir);

    *undo_description = g_strdup_printf (_("Restore original permissions of items enclosed in “%s”"), name);
    *redo_description = g_strdup_printf (_("Set permissions of items enclosed in “%s”"), name);

    *undo_label = g_strdup (_("_Undo Change Permissions"));
    *redo_label = g_strdup (_("_Redo Change Permissions"));

    g_free (name);
}

static void
rec_permissions_callback (gboolean success,
                          gpointer callback_data)
{
    file_undo_info_transfer_callback (NULL, success, callback_data);
}

static void
rec_permissions_redo_func (NautilusFileUndoInfo *info,
                           GtkWindow            *parent_window)
{
    NautilusFileUndoInfoRecPermissions *self = NAUTILUS_FILE_UNDO_INFO_REC_PERMISSIONS (info);
    gchar *parent_uri;

    parent_uri = g_file_get_uri (self->priv->dest_dir);
    nautilus_file_set_permissions_recursive (parent_uri,
                                             self->priv->file_permissions,
                                             self->priv->file_mask,
                                             self->priv->dir_permissions,
                                             self->priv->dir_mask,
                                             rec_permissions_callback, self);
    g_free (parent_uri);
}

static void
rec_permissions_undo_func (NautilusFileUndoInfo *info,
                           GtkWindow            *parent_window)
{
    NautilusFileUndoInfoRecPermissions *self = NAUTILUS_FILE_UNDO_INFO_REC_PERMISSIONS (info);

    if (g_hash_table_size (self->priv->original_permissions) > 0)
    {
        GList *gfiles_list;
        guint32 perm;
        GList *l;
        GFile *dest;
        char *item;

        gfiles_list = g_hash_table_get_keys (self->priv->original_permissions);
        for (l = gfiles_list; l != NULL; l = l->next)
        {
            item = l->data;
            perm = GPOINTER_TO_UINT (g_hash_table_lookup (self->priv->original_permissions, item));
            dest = g_file_new_for_uri (item);
            g_file_set_attribute_uint32 (dest,
                                         G_FILE_ATTRIBUTE_UNIX_MODE,
                                         perm, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
            g_object_unref (dest);
        }

        g_list_free (gfiles_list);
        /* Here we must do what's necessary for the callback */
        file_undo_info_transfer_callback (NULL, TRUE, self);
    }
}

static void
nautilus_file_undo_info_rec_permissions_init (NautilusFileUndoInfoRecPermissions *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, nautilus_file_undo_info_rec_permissions_get_type (),
                                              NautilusFileUndoInfoRecPermissionsDetails);

    self->priv->original_permissions =
        g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
}

static void
nautilus_file_undo_info_rec_permissions_finalize (GObject *obj)
{
    NautilusFileUndoInfoRecPermissions *self = NAUTILUS_FILE_UNDO_INFO_REC_PERMISSIONS (obj);

    g_hash_table_destroy (self->priv->original_permissions);
    g_clear_object (&self->priv->dest_dir);

    G_OBJECT_CLASS (nautilus_file_undo_info_rec_permissions_parent_class)->finalize (obj);
}

static void
nautilus_file_undo_info_rec_permissions_class_init (NautilusFileUndoInfoRecPermissionsClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS (klass);
    NautilusFileUndoInfoClass *iclass = NAUTILUS_FILE_UNDO_INFO_CLASS (klass);

    oclass->finalize = nautilus_file_undo_info_rec_permissions_finalize;

    iclass->undo_func = rec_permissions_undo_func;
    iclass->redo_func = rec_permissions_redo_func;
    iclass->strings_func = rec_permissions_strings_func;

    g_type_class_add_private (klass, sizeof (NautilusFileUndoInfoRecPermissionsDetails));
}

NautilusFileUndoInfo *
nautilus_file_undo_info_rec_permissions_new (GFile   *dest,
                                             guint32  file_permissions,
                                             guint32  file_mask,
                                             guint32  dir_permissions,
                                             guint32  dir_mask)
{
    NautilusFileUndoInfoRecPermissions *retval;

    retval = g_object_new (NAUTILUS_TYPE_FILE_UNDO_INFO_REC_PERMISSIONS,
                           "op-type", NAUTILUS_FILE_UNDO_OP_RECURSIVE_SET_PERMISSIONS,
                           "item-count", 1,
                           NULL);

    retval->priv->dest_dir = g_object_ref (dest);
    retval->priv->file_permissions = file_permissions;
    retval->priv->file_mask = file_mask;
    retval->priv->dir_permissions = dir_permissions;
    retval->priv->dir_mask = dir_mask;

    return NAUTILUS_FILE_UNDO_INFO (retval);
}

void
nautilus_file_undo_info_rec_permissions_add_file (NautilusFileUndoInfoRecPermissions *self,
                                                  GFile                              *file,
                                                  guint32                             permission)
{
    gchar *original_uri = g_file_get_uri (file);
    g_hash_table_insert (self->priv->original_permissions, original_uri, GUINT_TO_POINTER (permission));
}

/* single file change permissions */
G_DEFINE_TYPE (NautilusFileUndoInfoPermissions, nautilus_file_undo_info_permissions, NAUTILUS_TYPE_FILE_UNDO_INFO)

struct _NautilusFileUndoInfoPermissionsDetails
{
    GFile *target_file;
    guint32 current_permissions;
    guint32 new_permissions;
};

static void
permissions_strings_func (NautilusFileUndoInfo  *info,
                          gchar                **undo_label,
                          gchar                **undo_description,
                          gchar                **redo_label,
                          gchar                **redo_description)
{
    NautilusFileUndoInfoPermissions *self = NAUTILUS_FILE_UNDO_INFO_PERMISSIONS (info);
    gchar *name;

    name = g_file_get_parse_name (self->priv->target_file);
    *undo_description = g_strdup_printf (_("Restore original permissions of “%s”"), name);
    *redo_description = g_strdup_printf (_("Set permissions of “%s”"), name);

    *undo_label = g_strdup (_("_Undo Change Permissions"));
    *redo_label = g_strdup (_("_Redo Change Permissions"));

    g_free (name);
}

static void
permissions_real_func (NautilusFileUndoInfoPermissions *self,
                       guint32                          permissions)
{
    NautilusFile *file;

    file = nautilus_file_get (self->priv->target_file);
    nautilus_file_set_permissions (file, permissions,
                                   file_undo_info_operation_callback, self);

    nautilus_file_unref (file);
}

static void
permissions_redo_func (NautilusFileUndoInfo *info,
                       GtkWindow            *parent_window)
{
    NautilusFileUndoInfoPermissions *self = NAUTILUS_FILE_UNDO_INFO_PERMISSIONS (info);
    permissions_real_func (self, self->priv->new_permissions);
}

static void
permissions_undo_func (NautilusFileUndoInfo *info,
                       GtkWindow            *parent_window)
{
    NautilusFileUndoInfoPermissions *self = NAUTILUS_FILE_UNDO_INFO_PERMISSIONS (info);
    permissions_real_func (self, self->priv->current_permissions);
}

static void
nautilus_file_undo_info_permissions_init (NautilusFileUndoInfoPermissions *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, nautilus_file_undo_info_permissions_get_type (),
                                              NautilusFileUndoInfoPermissionsDetails);
}

static void
nautilus_file_undo_info_permissions_finalize (GObject *obj)
{
    NautilusFileUndoInfoPermissions *self = NAUTILUS_FILE_UNDO_INFO_PERMISSIONS (obj);
    g_clear_object (&self->priv->target_file);

    G_OBJECT_CLASS (nautilus_file_undo_info_permissions_parent_class)->finalize (obj);
}

static void
nautilus_file_undo_info_permissions_class_init (NautilusFileUndoInfoPermissionsClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS (klass);
    NautilusFileUndoInfoClass *iclass = NAUTILUS_FILE_UNDO_INFO_CLASS (klass);

    oclass->finalize = nautilus_file_undo_info_permissions_finalize;

    iclass->undo_func = permissions_undo_func;
    iclass->redo_func = permissions_redo_func;
    iclass->strings_func = permissions_strings_func;

    g_type_class_add_private (klass, sizeof (NautilusFileUndoInfoPermissionsDetails));
}

NautilusFileUndoInfo *
nautilus_file_undo_info_permissions_new (GFile   *file,
                                         guint32  current_permissions,
                                         guint32  new_permissions)
{
    NautilusFileUndoInfoPermissions *retval;

    retval = g_object_new (NAUTILUS_TYPE_FILE_UNDO_INFO_PERMISSIONS,
                           "op-type", NAUTILUS_FILE_UNDO_OP_SET_PERMISSIONS,
                           "item-count", 1,
                           NULL);

    retval->priv->target_file = g_object_ref (file);
    retval->priv->current_permissions = current_permissions;
    retval->priv->new_permissions = new_permissions;

    return NAUTILUS_FILE_UNDO_INFO (retval);
}

/* group and owner change */
G_DEFINE_TYPE (NautilusFileUndoInfoOwnership, nautilus_file_undo_info_ownership, NAUTILUS_TYPE_FILE_UNDO_INFO)

struct _NautilusFileUndoInfoOwnershipDetails
{
    GFile *target_file;
    char *original_ownership;
    char *new_ownership;
};

static void
ownership_strings_func (NautilusFileUndoInfo  *info,
                        gchar                **undo_label,
                        gchar                **undo_description,
                        gchar                **redo_label,
                        gchar                **redo_description)
{
    NautilusFileUndoInfoOwnership *self = NAUTILUS_FILE_UNDO_INFO_OWNERSHIP (info);
    NautilusFileUndoOp op_type = nautilus_file_undo_info_get_op_type (info);
    gchar *name;

    name = g_file_get_parse_name (self->priv->target_file);

    if (op_type == NAUTILUS_FILE_UNDO_OP_CHANGE_OWNER)
    {
        *undo_description = g_strdup_printf (_("Restore group of “%s” to “%s”"),
                                             name, self->priv->original_ownership);
        *redo_description = g_strdup_printf (_("Set group of “%s” to “%s”"),
                                             name, self->priv->new_ownership);

        *undo_label = g_strdup (_("_Undo Change Group"));
        *redo_label = g_strdup (_("_Redo Change Group"));
    }
    else if (op_type == NAUTILUS_FILE_UNDO_OP_CHANGE_GROUP)
    {
        *undo_description = g_strdup_printf (_("Restore owner of “%s” to “%s”"),
                                             name, self->priv->original_ownership);
        *redo_description = g_strdup_printf (_("Set owner of “%s” to “%s”"),
                                             name, self->priv->new_ownership);

        *undo_label = g_strdup (_("_Undo Change Owner"));
        *redo_label = g_strdup (_("_Redo Change Owner"));
    }

    g_free (name);
}

static void
ownership_real_func (NautilusFileUndoInfoOwnership *self,
                     const gchar                   *ownership)
{
    NautilusFileUndoOp op_type = nautilus_file_undo_info_get_op_type (NAUTILUS_FILE_UNDO_INFO (self));
    NautilusFile *file;

    file = nautilus_file_get (self->priv->target_file);

    if (op_type == NAUTILUS_FILE_UNDO_OP_CHANGE_OWNER)
    {
        nautilus_file_set_owner (file,
                                 ownership,
                                 file_undo_info_operation_callback, self);
    }
    else if (op_type == NAUTILUS_FILE_UNDO_OP_CHANGE_GROUP)
    {
        nautilus_file_set_group (file,
                                 ownership,
                                 file_undo_info_operation_callback, self);
    }

    nautilus_file_unref (file);
}

static void
ownership_redo_func (NautilusFileUndoInfo *info,
                     GtkWindow            *parent_window)
{
    NautilusFileUndoInfoOwnership *self = NAUTILUS_FILE_UNDO_INFO_OWNERSHIP (info);
    ownership_real_func (self, self->priv->new_ownership);
}

static void
ownership_undo_func (NautilusFileUndoInfo *info,
                     GtkWindow            *parent_window)
{
    NautilusFileUndoInfoOwnership *self = NAUTILUS_FILE_UNDO_INFO_OWNERSHIP (info);
    ownership_real_func (self, self->priv->original_ownership);
}

static void
nautilus_file_undo_info_ownership_init (NautilusFileUndoInfoOwnership *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, nautilus_file_undo_info_ownership_get_type (),
                                              NautilusFileUndoInfoOwnershipDetails);
}

static void
nautilus_file_undo_info_ownership_finalize (GObject *obj)
{
    NautilusFileUndoInfoOwnership *self = NAUTILUS_FILE_UNDO_INFO_OWNERSHIP (obj);

    g_clear_object (&self->priv->target_file);
    g_free (self->priv->original_ownership);
    g_free (self->priv->new_ownership);

    G_OBJECT_CLASS (nautilus_file_undo_info_ownership_parent_class)->finalize (obj);
}

static void
nautilus_file_undo_info_ownership_class_init (NautilusFileUndoInfoOwnershipClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS (klass);
    NautilusFileUndoInfoClass *iclass = NAUTILUS_FILE_UNDO_INFO_CLASS (klass);

    oclass->finalize = nautilus_file_undo_info_ownership_finalize;

    iclass->undo_func = ownership_undo_func;
    iclass->redo_func = ownership_redo_func;
    iclass->strings_func = ownership_strings_func;

    g_type_class_add_private (klass, sizeof (NautilusFileUndoInfoOwnershipDetails));
}

NautilusFileUndoInfo *
nautilus_file_undo_info_ownership_new (NautilusFileUndoOp  op_type,
                                       GFile              *file,
                                       const char         *current_data,
                                       const char         *new_data)
{
    NautilusFileUndoInfoOwnership *retval;

    retval = g_object_new (NAUTILUS_TYPE_FILE_UNDO_INFO_OWNERSHIP,
                           "item-count", 1,
                           "op-type", op_type,
                           NULL);

    retval->priv->target_file = g_object_ref (file);
    retval->priv->original_ownership = g_strdup (current_data);
    retval->priv->new_ownership = g_strdup (new_data);

    return NAUTILUS_FILE_UNDO_INFO (retval);
}

/* extract */
G_DEFINE_TYPE (NautilusFileUndoInfoExtract, nautilus_file_undo_info_extract, NAUTILUS_TYPE_FILE_UNDO_INFO)

struct _NautilusFileUndoInfoExtractDetails
{
    GList *sources;
    GFile *destination_directory;
    GList *outputs;
};

static void
extract_callback (GList    *outputs,
                  gpointer  callback_data)
{
    NautilusFileUndoInfoExtract *self = NAUTILUS_FILE_UNDO_INFO_EXTRACT (callback_data);
    gboolean success;

    nautilus_file_undo_info_extract_set_outputs (self, outputs);

    success = self->priv->outputs != NULL;

    file_undo_info_transfer_callback (NULL, success, self);
}

static void
extract_strings_func (NautilusFileUndoInfo  *info,
                      gchar                **undo_label,
                      gchar                **undo_description,
                      gchar                **redo_label,
                      gchar                **redo_description)
{
    NautilusFileUndoInfoExtract *self = NAUTILUS_FILE_UNDO_INFO_EXTRACT (info);
    gint total_sources;
    gint total_outputs;

    *undo_label = g_strdup (_("_Undo Extract"));
    *redo_label = g_strdup (_("_Redo Extract"));

    total_sources = g_list_length (self->priv->sources);
    total_outputs = g_list_length (self->priv->outputs);

    if (total_outputs == 1)
    {
        GFile *output;
        g_autofree gchar *name = NULL;

        output = self->priv->outputs->data;
        name = g_file_get_parse_name (output);

        *undo_description = g_strdup_printf (_("Delete “%s”"), name);
    }
    else
    {
        *undo_description = g_strdup_printf (ngettext ("Delete %d extracted file",
                                                       "Delete %d extracted files",
                                                       total_outputs),
                                             total_outputs);
    }

    if (total_sources == 1)
    {
        GFile *source;
        g_autofree gchar *name = NULL;

        source = self->priv->sources->data;
        name = g_file_get_parse_name (source);

        *redo_description = g_strdup_printf (_("Extract “%s”"), name);
    }
    else
    {
        *redo_description = g_strdup_printf (ngettext ("Extract %d file",
                                                       "Extract %d files",
                                                       total_sources),
                                             total_sources);
    }
}

static void
extract_redo_func (NautilusFileUndoInfo *info,
                   GtkWindow            *parent_window)
{
    NautilusFileUndoInfoExtract *self = NAUTILUS_FILE_UNDO_INFO_EXTRACT (info);

    nautilus_file_operations_extract_files (self->priv->sources,
                                            self->priv->destination_directory,
                                            parent_window,
                                            extract_callback,
                                            self);
}

static void
extract_undo_func (NautilusFileUndoInfo *info,
                   GtkWindow            *parent_window)
{
    NautilusFileUndoInfoExtract *self = NAUTILUS_FILE_UNDO_INFO_EXTRACT (info);

    nautilus_file_operations_delete (self->priv->outputs, parent_window,
                                     file_undo_info_delete_callback, self);
}

static void
nautilus_file_undo_info_extract_init (NautilusFileUndoInfoExtract *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, nautilus_file_undo_info_extract_get_type (),
                                              NautilusFileUndoInfoExtractDetails);
}

static void
nautilus_file_undo_info_extract_finalize (GObject *obj)
{
    NautilusFileUndoInfoExtract *self = NAUTILUS_FILE_UNDO_INFO_EXTRACT (obj);

    g_object_unref (self->priv->destination_directory);
    g_list_free_full (self->priv->sources, g_object_unref);
    if (self->priv->outputs)
    {
        g_list_free_full (self->priv->outputs, g_object_unref);
    }

    G_OBJECT_CLASS (nautilus_file_undo_info_extract_parent_class)->finalize (obj);
}

static void
nautilus_file_undo_info_extract_class_init (NautilusFileUndoInfoExtractClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS (klass);
    NautilusFileUndoInfoClass *iclass = NAUTILUS_FILE_UNDO_INFO_CLASS (klass);

    oclass->finalize = nautilus_file_undo_info_extract_finalize;

    iclass->undo_func = extract_undo_func;
    iclass->redo_func = extract_redo_func;
    iclass->strings_func = extract_strings_func;

    g_type_class_add_private (klass, sizeof (NautilusFileUndoInfoExtractDetails));
}

void
nautilus_file_undo_info_extract_set_outputs (NautilusFileUndoInfoExtract *self,
                                             GList                       *outputs)
{
    if (self->priv->outputs)
    {
        g_list_free_full (self->priv->outputs, g_object_unref);
    }
    self->priv->outputs = g_list_copy_deep (outputs,
                                            (GCopyFunc) g_object_ref,
                                            NULL);
}

NautilusFileUndoInfo *
nautilus_file_undo_info_extract_new (GList *sources,
                                     GFile *destination_directory)
{
    NautilusFileUndoInfoExtract *self;

    self = g_object_new (NAUTILUS_TYPE_FILE_UNDO_INFO_EXTRACT,
                         "item-count", 1,
                         "op-type", NAUTILUS_FILE_UNDO_OP_EXTRACT,
                         NULL);

    self->priv->sources = g_list_copy_deep (sources,
                                            (GCopyFunc) g_object_ref,
                                            NULL);
    self->priv->destination_directory = g_object_ref (destination_directory);

    return NAUTILUS_FILE_UNDO_INFO (self);
}


/* compress */
G_DEFINE_TYPE (NautilusFileUndoInfoCompress, nautilus_file_undo_info_compress, NAUTILUS_TYPE_FILE_UNDO_INFO)

struct _NautilusFileUndoInfoCompressDetails
{
    GList *sources;
    GFile *output;
    AutoarFormat format;
    AutoarFilter filter;
};

static void
compress_callback (GFile    *new_file,
                   gboolean  success,
                   gpointer  callback_data)
{
    NautilusFileUndoInfoCompress *self = NAUTILUS_FILE_UNDO_INFO_COMPRESS (callback_data);

    if (success)
    {
        g_object_unref (self->priv->output);

        self->priv->output = g_object_ref (new_file);
    }

    file_undo_info_transfer_callback (NULL, success, self);
}

static void
compress_strings_func (NautilusFileUndoInfo  *info,
                       gchar                **undo_label,
                       gchar                **undo_description,
                       gchar                **redo_label,
                       gchar                **redo_description)
{
    NautilusFileUndoInfoCompress *self = NAUTILUS_FILE_UNDO_INFO_COMPRESS (info);
    g_autofree gchar *output_name = NULL;
    gint sources_count;

    output_name = g_file_get_parse_name (self->priv->output);
    *undo_description = g_strdup_printf (_("Delete “%s”"), output_name);

    sources_count = g_list_length (self->priv->sources);
    if (sources_count == 1)
    {
        GFile *source;
        g_autofree gchar *source_name = NULL;

        source = self->priv->sources->data;
        source_name = g_file_get_parse_name (source);

        *redo_description = g_strdup_printf (_("Compress “%s”"), source_name);
    }
    else
    {
        *redo_description = g_strdup_printf (ngettext ("Compress %d file",
                                                       "Compress %d files",
                                                       sources_count),
                                             sources_count);
    }

    *undo_label = g_strdup (_("_Undo Compress"));
    *redo_label = g_strdup (_("_Redo Compress"));
}

static void
compress_redo_func (NautilusFileUndoInfo *info,
                    GtkWindow            *parent_window)
{
    NautilusFileUndoInfoCompress *self = NAUTILUS_FILE_UNDO_INFO_COMPRESS (info);

    nautilus_file_operations_compress (self->priv->sources,
                                       self->priv->output,
                                       self->priv->format,
                                       self->priv->filter,
                                       parent_window,
                                       compress_callback,
                                       self);
}

static void
compress_undo_func (NautilusFileUndoInfo *info,
                    GtkWindow            *parent_window)
{
    NautilusFileUndoInfoCompress *self = NAUTILUS_FILE_UNDO_INFO_COMPRESS (info);
    GList *files = NULL;

    files = g_list_prepend (files, self->priv->output);

    nautilus_file_operations_delete (files, parent_window,
                                     file_undo_info_delete_callback, self);

    g_list_free (files);
}

static void
nautilus_file_undo_info_compress_init (NautilusFileUndoInfoCompress *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, nautilus_file_undo_info_compress_get_type (),
                                              NautilusFileUndoInfoCompressDetails);
}

static void
nautilus_file_undo_info_compress_finalize (GObject *obj)
{
    NautilusFileUndoInfoCompress *self = NAUTILUS_FILE_UNDO_INFO_COMPRESS (obj);

    g_list_free_full (self->priv->sources, g_object_unref);
    g_clear_object (&self->priv->output);

    G_OBJECT_CLASS (nautilus_file_undo_info_compress_parent_class)->finalize (obj);
}

static void
nautilus_file_undo_info_compress_class_init (NautilusFileUndoInfoCompressClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS (klass);
    NautilusFileUndoInfoClass *iclass = NAUTILUS_FILE_UNDO_INFO_CLASS (klass);

    oclass->finalize = nautilus_file_undo_info_compress_finalize;

    iclass->undo_func = compress_undo_func;
    iclass->redo_func = compress_redo_func;
    iclass->strings_func = compress_strings_func;

    g_type_class_add_private (klass, sizeof (NautilusFileUndoInfoCompressDetails));
}

NautilusFileUndoInfo *
nautilus_file_undo_info_compress_new (GList        *sources,
                                      GFile        *output,
                                      AutoarFormat  format,
                                      AutoarFilter  filter)
{
    NautilusFileUndoInfoCompress *self;

    self = g_object_new (NAUTILUS_TYPE_FILE_UNDO_INFO_COMPRESS,
                         "item-count", 1,
                         "op-type", NAUTILUS_FILE_UNDO_OP_COMPRESS,
                         NULL);

    self->priv->sources = g_list_copy_deep (sources, (GCopyFunc) g_object_ref, NULL);
    self->priv->output = g_object_ref (output);
    self->priv->format = format;
    self->priv->filter = filter;

    return NAUTILUS_FILE_UNDO_INFO (self);
}
