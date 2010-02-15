/*
 * Copyright (C) 2009 Canonical Ltd.
 * Author: Robert Ancell <robert.ancell@canonical.com>
 * 
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See http://www.gnu.org/copyleft/gpl.html the full text of the
 * license.
 */

#include <string.h>
#include <sane/sane.h>
#include <sane/saneopts.h>
#include <glib/gi18n.h>

#include "scanner.h"


enum {
    UPDATE_DEVICES,
    AUTHORIZE,
    GOT_PAGE_INFO,
    GOT_LINE,
    SCAN_FAILED,
    PAGE_DONE,
    DOCUMENT_DONE,
    LAST_SIGNAL
};
static guint signals[LAST_SIGNAL] = { 0, };

typedef struct
{
    Scanner *instance;
    guint sig;
    gpointer data;
} SignalInfo;

typedef struct
{
    gchar *device;
    gint dpi;
    ScanMode scan_mode;
    gint depth;
    gboolean multi_page;
} ScanJob;

typedef struct
{
    enum
    {
       REQUEST_CANCEL,
       REQUEST_REDETECT,
       REQUEST_START_SCAN,
       REQUEST_QUIT
    } type;
    ScanJob *job;
} ScanRequest;

typedef struct
{
    gchar *username, *password;
} Credentials;

typedef enum
{
    STATE_IDLE = 0,
    STATE_REDETECT,
    STATE_OPEN,
    STATE_GET_OPTION,
    STATE_START,
    STATE_GET_PARAMETERS,
    STATE_READ
} ScanState;

struct ScannerPrivate
{
    GAsyncQueue *scan_queue, *authorize_queue;
    GThread *thread;

    ScanDevice *default_device;

    ScanState state;
    gboolean redetect;
  
    GList *job_queue;

    /* Handle to SANE device */
    SANE_Handle handle;
    gchar *current_device;

    SANE_Parameters parameters;

    /* Last option read */
    SANE_Int option_index;

    /* Buffer for received line */
    SANE_Byte *buffer;

    SANE_Int bytes_remaining, line_count, pass_number, page_number, notified_page;
  
    gboolean scanning;
};

G_DEFINE_TYPE (Scanner, scanner, G_TYPE_OBJECT);


/* Table of scanner objects for each thread (required for authorization callback) */
static GHashTable *scanners;


static gboolean
send_signal (SignalInfo *info)
{
    g_signal_emit (info->instance, signals[info->sig], 0, info->data);

    if (info->sig == DOCUMENT_DONE || info->sig == SCAN_FAILED)
        info->instance->priv->scanning = FALSE;

    switch (info->sig) {
    case UPDATE_DEVICES:
        {
            GList *iter, *devices = info->data;
            for (iter = devices; iter; iter = iter->next) {
                ScanDevice *device = iter->data;
                g_free (device->name);
                g_free (device->label);
                g_free (device);
            }
            g_list_free (devices);
        }
        break;
    case AUTHORIZE:
        {
            gchar *resource = info->data;
            g_free (resource);
        }
        break;
    case GOT_PAGE_INFO:
        {
            ScanPageInfo *page_info = info->data;
            g_free (page_info);
        }
        break;
    case GOT_LINE:
        {
            ScanLine *line = info->data;
            g_free(line->data);
            g_free(line);
        }
        break;
    case SCAN_FAILED:
        {
            GError *error = info->data;
            g_error_free (error);
        }
        break;
    default:
    case PAGE_DONE:
    case DOCUMENT_DONE:
    case LAST_SIGNAL:
        g_assert (info->data == NULL);
        break;
    }
    g_free (info);

    return FALSE;
}


/* Emit signals in main loop */
static void
emit_signal (Scanner *scanner, guint sig, gpointer data)
{
    SignalInfo *info;
    
    info = g_malloc(sizeof(SignalInfo));
    info->instance = scanner;
    info->sig = sig;
    info->data = data;
    g_idle_add ((GSourceFunc) send_signal, info);
}


static gint get_device_weight (const gchar *device)
{
    /* Use locally connected devices first */
    if (g_str_has_prefix (device, "usb:"))
       return 0;

    /* Use webcams as a last resort */
    if (g_str_has_prefix (device, "vfl:"))
       return 2;

    return 1;
}


static gint
compare_devices (ScanDevice *device1, ScanDevice *device2)
{
    gint weight1, weight2;
  
    /* Should do some fuzzy matching on the last selected device and set that to the default */

    weight1 = get_device_weight (device1->name);
    weight2 = get_device_weight (device2->name);
    if (weight1 != weight2)
        return weight1 - weight2;
   
    return strcmp (device1->label, device2->label);
}


static const char *
get_status_string (SANE_Status status)
{
    struct {
        SANE_Status status;
        const char *name;
    } status_names[] = {
        { SANE_STATUS_GOOD,          "SANE_STATUS_GOOD"},
        { SANE_STATUS_UNSUPPORTED,   "SANE_STATUS_UNSUPPORTED"},
        { SANE_STATUS_CANCELLED,     "SANE_STATUS_CANCELLED"},
        { SANE_STATUS_DEVICE_BUSY,   "SANE_STATUS_DEVICE_BUSY"},
        { SANE_STATUS_INVAL,         "SANE_STATUS_INVAL"},
        { SANE_STATUS_EOF,           "SANE_STATUS_EOF"},
        { SANE_STATUS_JAMMED,        "SANE_STATUS_JAMMED"},
        { SANE_STATUS_NO_DOCS,       "SANE_STATUS_NO_DOCS"},
        { SANE_STATUS_COVER_OPEN,    "SANE_STATUS_COVER_OPEN"},
        { SANE_STATUS_IO_ERROR,      "SANE_STATUS_IO_ERROR"},
        { SANE_STATUS_NO_MEM,        "SANE_STATUS_NO_MEM"},
        { SANE_STATUS_ACCESS_DENIED, "SANE_STATUS_ACCESS_DENIED"},
        { -1,                        NULL}
    };
    static char *unknown_string = NULL;
    int i;

    for (i = 0; status_names[i].name != NULL && status_names[i].status != status; i++);

    if (status_names[i].name == NULL) {
        g_free (unknown_string);
        unknown_string = g_strdup_printf ("SANE_STATUS(%d)", status);
        return unknown_string; /* Note result is undefined on second call to this function */
    }
  
    return status_names[i].name;
}


static const char *
get_action_string (SANE_Action action)
{
    struct {
        SANE_Action action;
        const char *name;
    } action_names[] = {
        { SANE_ACTION_GET_VALUE, "SANE_ACTION_GET_VALUE" },
        { SANE_ACTION_SET_VALUE, "SANE_ACTION_SET_VALUE" },
        { SANE_ACTION_SET_AUTO,  "SANE_ACTION_SET_AUTO" },
        { -1,                    NULL}
    };
    static char *unknown_string = NULL;
    int i;

    for (i = 0; action_names[i].name != NULL && action_names[i].action != action; i++);

    if (action_names[i].name == NULL) {
        g_free (unknown_string);
        unknown_string = g_strdup_printf ("SANE_ACTION(%d)", action);
        return unknown_string; /* Note result is undefined on second call to this function */
    }
  
    return action_names[i].name;
}


static const char *
get_frame_string (SANE_Frame frame)
{
    struct {
        SANE_Frame frame;
        const char *name;
    } frame_names[] = {
        { SANE_FRAME_GRAY,  "SANE_FRAME_GRAY" },
        { SANE_FRAME_RGB,   "SANE_FRAME_RGB" },      
        { SANE_FRAME_RED,   "SANE_FRAME_RED" },
        { SANE_FRAME_GREEN, "SANE_FRAME_GREEN" },
        { SANE_FRAME_BLUE,  "SANE_FRAME_BLUE" },
        { -1,               NULL}
    };
    static char *unknown_string = NULL;
    int i;

    for (i = 0; frame_names[i].name != NULL && frame_names[i].frame != frame; i++);

    if (frame_names[i].name == NULL) {
        g_free (unknown_string);
        unknown_string = g_strdup_printf ("SANE_FRAME(%d)", frame);
        return unknown_string; /* Note result is undefined on second call to this function */
    }
  
    return frame_names[i].name;
}


static void
do_redetect (Scanner *scanner)
{
    const SANE_Device **device_list, **device_iter;
    SANE_Status status;
    GList *devices = NULL;

    status = sane_get_devices (&device_list, SANE_FALSE);
    g_debug ("sane_get_devices () -> %s", get_status_string (status));
    if (status != SANE_STATUS_GOOD) {
        g_warning ("Unable to get SANE devices: %s", sane_strstatus(status));
        scanner->priv->state = STATE_IDLE;        
        return;
    }

    for (device_iter = device_list; *device_iter; device_iter++) {
        const SANE_Device *device = *device_iter;
        ScanDevice *scan_device;
        GString *label;
        gchar *c;

        g_debug ("Device: name=\"%s\" vendor=\"%s\" model=\"%s\" type=\"%s\"",
                 device->name, device->vendor, device->model, device->type);
        
        scan_device = g_malloc0 (sizeof (ScanDevice));

        scan_device->name = g_strdup (device->name);

        label = g_string_new ("");
        /* Abbreviate HP as it is a long string and does not match what is on the physical scanner */
        if (strcmp (device->vendor, "Hewlett-Packard") == 0)
            g_string_printf (label, "HP %s", device->model);
        else
            g_string_printf (label, "%s %s", device->vendor, device->model);
        
        /* Replace underscored in name */
        for (c = label->str; *c; c++)
            if (*c == '_')
                *c = ' ';

        scan_device->label = label->str;
        g_string_free (label, FALSE);

        devices = g_list_append (devices, scan_device);
    }

    /* Sort devices by priority */
    devices = g_list_sort (devices, (GCompareFunc) compare_devices);

    scanner->priv->redetect = FALSE;
    scanner->priv->state = STATE_IDLE;

    if (devices)
        scanner->priv->default_device = g_list_nth_data (devices, 0);
    else
        scanner->priv->default_device = NULL;

    emit_signal (scanner, UPDATE_DEVICES, devices);
}


static gboolean
control_option (SANE_Handle handle, SANE_Value_Type type, SANE_Int index, SANE_Action action, void *value)
{
    SANE_Status status;
    
    status = sane_control_option (handle, index, action, value, NULL);
    switch (type) {
    case SANE_TYPE_BOOL:
        g_debug ("sane_control_option (%d, %s, %s) -> %s",
                 index, get_action_string (action),
                 *((SANE_Bool *) value) ? "SANE_TRUE" : "SANE_FALSE",
                 get_status_string (status));
        break;
    case SANE_TYPE_INT:
        g_debug ("sane_control_option (%d, %s, %d) -> %s",
                 index, get_action_string (action),
                 *((SANE_Int *) value),
                 get_status_string (status));
        break;
    case SANE_TYPE_FIXED:
        g_debug ("sane_control_option (%d, %s, %f) -> %s",
                 index, get_action_string (action),
                 SANE_UNFIX (*((SANE_Fixed *) value)),
                 get_status_string (status));
        break;
    case SANE_TYPE_STRING:
        g_debug ("sane_control_option (%d, %s, \"%s\") -> %s",
                 index, get_action_string (action),
                 (char *) value,
                 get_status_string (status));
        break;
    default:
        break;
    }
  
    if (status != SANE_STATUS_GOOD)
        g_warning ("Error setting control option: %s", sane_strstatus(status));

    return status == SANE_STATUS_GOOD;
}


static void
set_default_option (SANE_Handle handle, SANE_Int option_index)
{
    SANE_Status status;
    
    status = sane_control_option (handle, option_index, SANE_ACTION_SET_AUTO, NULL, NULL);
    g_debug ("sane_control_option (%d, SANE_ACTION_SET_AUTO) -> %s",
             option_index, get_status_string (status));
    if (status != SANE_STATUS_GOOD)
        g_warning ("Error setting control option: %s", sane_strstatus(status));
}


static void
set_bool_option (SANE_Handle handle, const SANE_Option_Descriptor *option, SANE_Int option_index, SANE_Bool value)
{
    SANE_Bool v = value;
    g_return_if_fail (option->type == SANE_TYPE_BOOL);
    control_option (handle, SANE_TYPE_BOOL, option_index, SANE_ACTION_SET_VALUE, &v);
}


static void
set_int_option (SANE_Handle handle, const SANE_Option_Descriptor *option, SANE_Int option_index, SANE_Int value)
{
    SANE_Int v = value;

    g_return_if_fail (option->type == SANE_TYPE_INT);

    if (option->constraint_type == SANE_CONSTRAINT_RANGE) {
        if (option->constraint.range->quant)
            v *= option->constraint.range->quant;
        if (v < option->constraint.range->min)
            v = option->constraint.range->min;
        if (v > option->constraint.range->max)
            v = option->constraint.range->max;
    }
    control_option (handle, SANE_TYPE_INT, option_index, SANE_ACTION_SET_VALUE, &v);
}


static void
set_fixed_option (SANE_Handle handle, const SANE_Option_Descriptor *option, SANE_Int option_index, double value)
{
    SANE_Fixed v = SANE_FIX (value);

    g_return_if_fail (option->type == SANE_TYPE_FIXED);

    control_option (handle, SANE_TYPE_FIXED, option_index, SANE_ACTION_SET_VALUE, &v);
}


static gboolean
set_string_option (SANE_Handle handle, const SANE_Option_Descriptor *option, SANE_Int option_index, const char *value)
{
    char *string;
    gsize value_size, size;
    gboolean result;

    g_return_val_if_fail (option->type == SANE_TYPE_STRING, FALSE);
    
    value_size = strlen (value) + 1;
    size = option->size > value_size ? option->size : value_size;
    string = g_malloc(sizeof(char) * size);
    strcpy (string, value);
    result = control_option (handle, SANE_TYPE_STRING, option_index, SANE_ACTION_SET_VALUE, string);
    g_free (string);
   
    return result;
}


static gboolean
set_constrained_string_option (SANE_Handle handle, const SANE_Option_Descriptor *option, SANE_Int option_index, const char *values[])
{
    gint i, j;

    g_return_val_if_fail (option->type == SANE_TYPE_STRING, FALSE);  
    g_return_val_if_fail (option->constraint_type == SANE_CONSTRAINT_STRING_LIST, FALSE);
  
    for (i = 0; values[i] != NULL; i++) {
        for (j = 0; option->constraint.string_list[j]; j++) {
            if (strcmp (values[i], option->constraint.string_list[j]) == 0)
               break;
        }

        if (option->constraint.string_list[j] != NULL)
            return set_string_option (handle, option, option_index, values[i]);
    }
  
    return FALSE;
}


static void
log_option (SANE_Int index, const SANE_Option_Descriptor *option)
{
    GString *string;
    SANE_Word i;
    SANE_Int cap;
    
    string = g_string_new ("");

    g_string_append_printf (string, "Option %d:", index);
    
    if (option->name)    
        g_string_append_printf (string, " name='%s'", option->name);
    
    if (option->title)
        g_string_append_printf (string, " title='%s'", option->title);

    switch (option->type) {
    case SANE_TYPE_BOOL:
        g_string_append (string, " type=bool");
        break;
    case SANE_TYPE_INT:
        g_string_append (string, " type=int");
        break;
    case SANE_TYPE_FIXED:
        g_string_append (string, " type=fixed");        
        break;
    case SANE_TYPE_STRING:
        g_string_append (string, " type=string");        
        break;
    case SANE_TYPE_BUTTON:
        g_string_append (string, " type=button");        
        break;
    case SANE_TYPE_GROUP:
        g_string_append (string, " type=group");
        break;
    default:
        g_string_append_printf (string, " type=%d", option->type);
        break;
    }
    
    g_string_append_printf (string, " size=%d", option->size);

    switch (option->unit) {
    case SANE_UNIT_NONE:
        break;
    case SANE_UNIT_PIXEL:
        g_string_append (string, " unit=pixels");
        break;
    case SANE_UNIT_BIT:
        g_string_append (string, " unit=bits");
        break;
    case SANE_UNIT_MM:
        g_string_append (string, " unit=mm");
        break;
    case SANE_UNIT_DPI:
        g_string_append (string, " unit=dpi");
        break;
    case SANE_UNIT_PERCENT:
        g_string_append (string, " unit=percent");
        break;
    case SANE_UNIT_MICROSECOND:
        g_string_append (string, " unit=microseconds");
        break;
    default:
        g_string_append_printf (string, " unit=%d", option->unit);
        break;
    }

    switch (option->constraint_type) {
    case SANE_CONSTRAINT_RANGE:
        if (option->type == SANE_TYPE_FIXED)
            g_string_append_printf (string, " min=%f, max=%f, quant=%d",
                                    SANE_UNFIX (option->constraint.range->min), SANE_UNFIX (option->constraint.range->max),
                                    option->constraint.range->quant);
        else
            g_string_append_printf (string, " min=%d, max=%d, quant=%d",
                                    option->constraint.range->min, option->constraint.range->max,
                                    option->constraint.range->quant);
        break;
    case SANE_CONSTRAINT_WORD_LIST:
        g_string_append (string, " values=[");
        for (i = 0; i < option->constraint.word_list[0]; i++) {
            if (i != 0)
                g_string_append (string, ", ");
            g_string_append_printf (string, "%d", option->constraint.word_list[i+1]);
        }
        g_string_append (string, "]");
        break;
    case SANE_CONSTRAINT_STRING_LIST:
        g_string_append (string, " values=[");
        for (i = 0; option->constraint.string_list[i]; i++) {
            if (i != 0)
                g_string_append (string, ", ");
            g_string_append_printf (string, "\"%s\"", option->constraint.string_list[i]);
        }
        g_string_append (string, "]");
        break;
    default:
        break;
    }
    
    cap = option->cap;
    if (cap) {
        struct {
            SANE_Int cap;
            const char *name;
        } caps[] = {
            { SANE_CAP_SOFT_SELECT,     "soft-select"},
            { SANE_CAP_HARD_SELECT,     "hard-select"},
            { SANE_CAP_SOFT_DETECT,     "soft-detect"},
            { SANE_CAP_EMULATED,        "emulated"},
            { SANE_CAP_AUTOMATIC,       "automatic"},
            { SANE_CAP_INACTIVE,        "inactive"},
            { SANE_CAP_ADVANCED,        "advanced"},
            { 0,                        NULL}
        };
        int i, n = 0;
        
        g_string_append (string, " cap=");
        for (i = 0; caps[i].cap > 0; i++) {
            if (cap & caps[i].cap) {
                cap &= ~caps[i].cap;
                if (n != 0)
                    g_string_append (string, ",");
                g_string_append (string, caps[i].name);
                n++;
            }
        }
        /* Unknown capabilities */
        if (cap) {
            if (n != 0)
                g_string_append (string, ",");
            g_string_append_printf (string, "%x", cap);
        }
    }

    g_debug ("%s", string->str);
    g_string_free (string, TRUE);

    if (option->desc)
        g_debug ("  Description: %s", option->desc);
}


static void
authorization_cb (SANE_String_Const resource, SANE_Char username[SANE_MAX_USERNAME_LEN], SANE_Char password[SANE_MAX_PASSWORD_LEN])
{
    Scanner *scanner;
    Credentials *credentials;
   
    scanner = g_hash_table_lookup (scanners, g_thread_self ());

    emit_signal (scanner, AUTHORIZE, g_strdup (resource));

    credentials = g_async_queue_pop (scanner->priv->authorize_queue);
    strncpy (username, credentials->username, SANE_MAX_USERNAME_LEN);
    strncpy (password, credentials->password, SANE_MAX_PASSWORD_LEN);
    g_free (credentials);
}


void
scanner_authorize (Scanner *scanner, const gchar *username, const gchar *password)
{
    Credentials *credentials;

    credentials = g_malloc (sizeof (Credentials));
    credentials->username = g_strdup (username);
    credentials->password = g_strdup (password);
    g_async_queue_push (scanner->priv->authorize_queue, credentials);
}


static void
close_device (Scanner *scanner)
{
    GList *iter;

    if (scanner->priv->handle) {
        sane_cancel (scanner->priv->handle);
        g_debug ("sane_cancel ()");

        sane_close (scanner->priv->handle);
        g_debug ("sane_close ()");
        scanner->priv->handle = NULL;
    }
  
    g_free (scanner->priv->buffer);
    scanner->priv->buffer = NULL;

    for (iter = scanner->priv->job_queue; iter; iter = iter->next) {
        ScanJob *job = (ScanJob *) iter->data;
        g_free (job->device);
        g_free (job);
    }
    g_list_free (scanner->priv->job_queue);
    scanner->priv->job_queue = NULL;
}


static gboolean
handle_requests (Scanner *scanner)
{
    gint request_count = 0;
  
    /* Redetect when idle */
    if (scanner->priv->state == STATE_IDLE && scanner->priv->redetect)
        scanner->priv->state = STATE_REDETECT;
  
    /* Process all requests */
    while (TRUE) {
        ScanRequest *request = NULL;

        if ((scanner->priv->state == STATE_IDLE && request_count == 0) ||
            g_async_queue_length (scanner->priv->scan_queue) > 0)
            request = g_async_queue_pop (scanner->priv->scan_queue);
        else
            return TRUE;

         g_debug ("Processing request");
         request_count++;

         switch (request->type) {
         case REQUEST_REDETECT:
             break;

         case REQUEST_START_SCAN:
             scanner->priv->job_queue = g_list_append (scanner->priv->job_queue, request->job);
             if (g_list_length (scanner->priv->job_queue) == 1)
                 scanner->priv->state = STATE_OPEN;
             break;

         case REQUEST_CANCEL:
             close_device (scanner);
             scanner->priv->state = STATE_IDLE;
             emit_signal (scanner, SCAN_FAILED,
                          g_error_new (SCANNER_TYPE, SANE_STATUS_CANCELLED, "Scan cancelled - do not report this error"));
             break;

         case REQUEST_QUIT:
             close_device (scanner);
             g_free (request);
             return FALSE;
         }

         g_free (request);
    }
  
    return TRUE;
}


static void
do_open (Scanner *scanner)
{
    SANE_Status status;
    ScanJob *job;

    job = (ScanJob *) scanner->priv->job_queue->data;

    scanner->priv->line_count = 0;
    scanner->priv->pass_number = 0;
    scanner->priv->page_number = 0;
    scanner->priv->notified_page = -1;
    scanner->priv->option_index = 0;

    if (!job->device)
        job->device = g_strdup (scanner->priv->default_device->name);

    if (!job->device) {
        scanner->priv->state = STATE_IDLE;
        g_warning ("No scan device available");
        scanner->priv->state = STATE_IDLE;
        emit_signal (scanner, SCAN_FAILED,
                     g_error_new (SCANNER_TYPE, status,
                                  /* Error displayed when no scanners to scan with */
                                  _("No scanners available.  Please connect a scanner.")));
        return;
    }
 
    /* See if we can use the already open device */
    if (scanner->priv->handle) {
        if (strcmp (scanner->priv->current_device, job->device) == 0) {
            scanner->priv->state = STATE_GET_OPTION;
            return;
        }

        sane_close (scanner->priv->handle);
        g_debug ("sane_close ()");
        scanner->priv->handle = NULL;
    }
  
    g_free (scanner->priv->current_device);
    scanner->priv->current_device = NULL;
  
    status = sane_open (job->device, &scanner->priv->handle);
    g_debug ("sane_open (\"%s\") -> %s", job->device, get_status_string (status));

    if (status != SANE_STATUS_GOOD) {
        g_warning ("Unable to get open device: %s", sane_strstatus (status));
        scanner->priv->handle = NULL;
        close_device (scanner);
        scanner->priv->state = STATE_IDLE;
        emit_signal (scanner, SCAN_FAILED,
                     g_error_new (SCANNER_TYPE, status,
                                  /* Error displayed when cannot connect to scanner */
                                  _("Unable to connect to scanner")));
        return;
    }

    scanner->priv->current_device = g_strdup (job->device);
    scanner->priv->state = STATE_GET_OPTION;
}


static void
do_get_option (Scanner *scanner)
{
    const SANE_Option_Descriptor *option;
    SANE_Int option_index;
    ScanJob *job;
  
    job = (ScanJob *) scanner->priv->job_queue->data;  

    option = sane_get_option_descriptor (scanner->priv->handle, scanner->priv->option_index);
    g_debug ("sane_get_option_descriptor (%d)", scanner->priv->option_index);
    option_index = scanner->priv->option_index;
    scanner->priv->option_index++;

    if (!option) {
        scanner->priv->state = STATE_START;
        return;
    }

    log_option (option_index, option);
    if (!option->name)
        return;

    if (strcmp (option->name, SANE_NAME_SCAN_RESOLUTION) == 0) {
        if (option->type == SANE_TYPE_FIXED)
            set_fixed_option (scanner->priv->handle, option, option_index, job->dpi);
        else
            set_int_option (scanner->priv->handle, option, option_index, job->dpi);
    }
    else if (strcmp (option->name, SANE_NAME_SCAN_SOURCE) == 0) {
        const char *adf_sources[] =
        {
            "Automatic Document Feeder",
            SANE_I18N ("Automatic Document Feeder"),
            "ADF",
            SANE_I18N ("ADF Front"),
            "ADF Front",
            NULL
        };

        if (job->multi_page) {
            if (!set_constrained_string_option (scanner->priv->handle, option, option_index, adf_sources))
                 g_warning ("Unable to set ADF source, please file a bug");
        }
        else {
            set_default_option (scanner->priv->handle, option_index);
        }
    }
    else if (strcmp (option->name, SANE_NAME_BIT_DEPTH) == 0) {
        if (job->depth > 0)
            set_int_option (scanner->priv->handle, option, option_index, job->depth);
    }
    else if (strcmp (option->name, SANE_NAME_SCAN_MODE) == 0) {
        /* The names of scan modes often used in drivers, as taken from the sane-backends source */
        const char *color_scan_modes[] =
        {
            SANE_VALUE_SCAN_MODE_COLOR,
            "Color",
            NULL
        };
        const char *gray_scan_modes[] =
        {
            SANE_VALUE_SCAN_MODE_GRAY,
            "Gray",
            "Grayscale",
            SANE_I18N ("Grayscale"),
            NULL
        };
        const char *lineart_scan_modes[] =
        {
            SANE_VALUE_SCAN_MODE_LINEART,
            "Lineart",
            "LineArt",
            SANE_I18N ("LineArt"),
            "Black & White",
            SANE_I18N ("Black & White"),
            "Binary",
            SANE_I18N ("Binary"),
            "Thresholded",
            SANE_VALUE_SCAN_MODE_GRAY,
            "Gray",
             "Grayscale",
            SANE_I18N ("Grayscale"),
            NULL
        };
            
        switch (job->scan_mode) {
        case SCAN_MODE_COLOR:
            if (!set_constrained_string_option (scanner->priv->handle, option, option_index, color_scan_modes))
                g_warning ("Unable to set Color mode, please file a bug");
            break;
        case SCAN_MODE_GRAY:
            if (!set_constrained_string_option (scanner->priv->handle, option, option_index, gray_scan_modes))
                g_warning ("Unable to set Gray mode, please file a bug");
            break;
        case SCAN_MODE_LINEART:
            if (!set_constrained_string_option (scanner->priv->handle, option, option_index, lineart_scan_modes))
                g_warning ("Unable to set Lineart mode, please file a bug");
            break;
        default:
            break;
        }
    }
    /* Disable compression, we will compress after scanning */
    else if (strcmp (option->name, "compression") == 0) {
        const char *disable_compression_names[] =
        {
            SANE_I18N ("None"),
            SANE_I18N ("none"),
            "None",
            "none",
            NULL
        };
            
        if (!set_constrained_string_option (scanner->priv->handle, option, option_index, disable_compression_names))
            g_warning ("Unable to disable compression, please file a bug");
    }
    /* Always use maximum scan area - some scanners default to using partial areas.  This should be patched in sane-backends */
    else if (strcmp (option->name, SANE_NAME_SCAN_BR_X) == 0 ||
             strcmp (option->name, SANE_NAME_SCAN_BR_Y) == 0) {
        switch (option->constraint_type)
        {
        case SANE_CONSTRAINT_RANGE:
            if (option->type == SANE_TYPE_FIXED)
                set_fixed_option (scanner->priv->handle, option, option_index, SANE_UNFIX (option->constraint.range->max));
            else
                set_int_option (scanner->priv->handle, option, option_index, option->constraint.range->max);
            break;
        default:
            break;
        }
    }

    /* Test scanner options (hoping will not effect other scanners...) */
    if (strcmp (scanner->priv->current_device, "test") == 0) {
        if (strcmp (option->name, "hand-scanner") == 0) {
            set_bool_option (scanner->priv->handle, option, option_index, FALSE);
        }
        else if (strcmp (option->name, "three-pass") == 0) {
            set_bool_option (scanner->priv->handle, option, option_index, FALSE);
        }                    
        else if (strcmp (option->name, "test-picture") == 0) {
            set_string_option (scanner->priv->handle, option, option_index, "Color pattern");
        }
        else if (strcmp (option->name, "read-delay") == 0) {
            set_bool_option (scanner->priv->handle, option, option_index, TRUE);
        }
        else if (strcmp (option->name, "read-delay-duration") == 0) {
            set_int_option (scanner->priv->handle, option, option_index, 200000);
        }
    }
}


static void
do_complete_document (Scanner *scanner)
{
    ScanJob *job;

    job = (ScanJob *) scanner->priv->job_queue->data;
    g_free (job->device);
    g_free (job);
    scanner->priv->job_queue = g_list_remove_link (scanner->priv->job_queue, scanner->priv->job_queue);

    scanner->priv->state = STATE_IDLE;
  
    /* Continue onto the next job */
    if (scanner->priv->job_queue) {
        scanner->priv->state = STATE_OPEN;
        return;
    }

    /* Trigger timeout to close */
    // TODO

    emit_signal (scanner, DOCUMENT_DONE, NULL);
}


static void
do_start (Scanner *scanner)
{
    SANE_Status status;
  
    status = sane_start (scanner->priv->handle);
    g_debug ("sane_start (page=%d, pass=%d) -> %s", scanner->priv->page_number, scanner->priv->pass_number, get_status_string (status));
    if (status == SANE_STATUS_GOOD) {
        scanner->priv->state = STATE_GET_PARAMETERS;
    }
    else if (status == SANE_STATUS_NO_DOCS) {
        do_complete_document (scanner);
    }
    else {
        g_warning ("Unable to start device: %s", sane_strstatus (status));
        close_device (scanner);
        scanner->priv->state = STATE_IDLE;
        emit_signal (scanner, SCAN_FAILED,
                     g_error_new (SCANNER_TYPE, status,
                                  /* Error display when unable to start scan */
                                  _("Unable to start scan")));
    }
}


static void
do_get_parameters (Scanner *scanner)
{
    SANE_Status status;
    ScanPageInfo *info;
    ScanJob *job;

    status = sane_get_parameters (scanner->priv->handle, &scanner->priv->parameters);
    g_debug ("sane_get_parameters () -> %s", get_status_string (status));
    if (status != SANE_STATUS_GOOD) {
        g_warning ("Unable to get device parameters: %s", sane_strstatus (status));
        close_device (scanner);
        scanner->priv->state = STATE_IDLE;
        emit_signal (scanner, SCAN_FAILED,
                     g_error_new (SCANNER_TYPE, status,
                                  /* Error displayed when communication with scanner broken */
                                  _("Error communicating with scanner")));
        return;
    }
  
    job = (ScanJob *) scanner->priv->job_queue->data;

    g_debug ("Parameters: format=%s last_frame=%s bytes_per_line=%d pixels_per_line=%d lines=%d depth=%d",
             get_frame_string (scanner->priv->parameters.format),
             scanner->priv->parameters.last_frame ? "SANE_TRUE" : "SANE_FALSE",
             scanner->priv->parameters.bytes_per_line,
             scanner->priv->parameters.pixels_per_line,
             scanner->priv->parameters.lines,
             scanner->priv->parameters.depth);

    info = g_malloc(sizeof(ScanPageInfo));
    info->width = scanner->priv->parameters.pixels_per_line;
    info->height = scanner->priv->parameters.lines;
    info->depth = scanner->priv->parameters.depth;
    info->dpi = job->dpi;

    if (scanner->priv->page_number != scanner->priv->notified_page) {
        emit_signal (scanner, GOT_PAGE_INFO, info);
        scanner->priv->notified_page = scanner->priv->page_number;
    }

    /* Prepare for read */
    scanner->priv->bytes_remaining = scanner->priv->parameters.bytes_per_line;
    scanner->priv->buffer = g_malloc(sizeof(SANE_Byte) * scanner->priv->bytes_remaining);
    scanner->priv->line_count = 0;
    scanner->priv->pass_number = 0;
    scanner->priv->state = STATE_READ;
}


static void
do_complete_page (Scanner *scanner)
{
    ScanJob *job;
  
    job = (ScanJob *) scanner->priv->job_queue->data;

    sane_cancel (scanner->priv->handle);
    g_debug ("sane_cancel ()");

    /* If multi-pass then scan another page */
    if (!scanner->priv->parameters.last_frame) {
        scanner->priv->pass_number++;
        scanner->priv->state = STATE_START;
        return;
    }

    /* Go back for another page */
    if (job->multi_page) {
        scanner->priv->page_number++;
        scanner->priv->pass_number = 0;
        emit_signal (scanner, PAGE_DONE, NULL);
        scanner->priv->state = STATE_START;
        return;
    }

    emit_signal (scanner, PAGE_DONE, NULL);  
    do_complete_document (scanner);
}


static void
do_read (Scanner *scanner)
{
    SANE_Status status;
    SANE_Int n_read;

    status = sane_read (scanner->priv->handle,
                        scanner->priv->buffer + (scanner->priv->parameters.bytes_per_line - scanner->priv->bytes_remaining),
                        scanner->priv->bytes_remaining,
                        &n_read);
    g_debug ("sane_read (%d) -> (%s, %d)", scanner->priv->bytes_remaining, get_status_string (status), n_read);

    /* End of variable length frame */
    if (status == SANE_STATUS_EOF &&
        scanner->priv->parameters.lines == -1 &&
        scanner->priv->bytes_remaining == scanner->priv->parameters.bytes_per_line) {
        do_complete_page (scanner);
        return;
    }
            
    /* Communication error */
    if (status != SANE_STATUS_GOOD) {
        g_warning ("Unable to read frame from device: %s", sane_strstatus (status));
        close_device (scanner);
        scanner->priv->state = STATE_IDLE;
        emit_signal (scanner, SCAN_FAILED,
                     g_error_new (SCANNER_TYPE, status,
                                  /* Error displayed when communication with scanner broken */
                                  _("Error communicating with scanner")));
        return;
    }
 
    scanner->priv->bytes_remaining -= n_read;
    if (scanner->priv->bytes_remaining == 0) {
        ScanLine *line;

        line = g_malloc(sizeof(ScanLine));
        switch (scanner->priv->parameters.format) {
        case SANE_FRAME_GRAY:
            line->format = LINE_GRAY;
            break;
        case SANE_FRAME_RGB:
            line->format = LINE_RGB;
            break;
        case SANE_FRAME_RED:
            line->format = LINE_RED;
            break;
        case SANE_FRAME_GREEN:
            line->format = LINE_GREEN;
            break;
        case SANE_FRAME_BLUE:
            line->format = LINE_BLUE;
            break;
        }
        line->width = scanner->priv->parameters.pixels_per_line;
        line->depth = scanner->priv->parameters.depth;
        line->data = scanner->priv->buffer;
        line->data_length = scanner->priv->parameters.bytes_per_line;
        line->number = scanner->priv->line_count;
        emit_signal (scanner, GOT_LINE, line);
        scanner->priv->buffer = NULL;

        /* On last line */
        scanner->priv->line_count++;
        if (scanner->priv->parameters.lines > 0 && scanner->priv->line_count == scanner->priv->parameters.lines) {
            do_complete_page (scanner);
        }
        else {
            scanner->priv->bytes_remaining = scanner->priv->parameters.bytes_per_line;
            scanner->priv->buffer = g_malloc(sizeof(SANE_Byte) * scanner->priv->bytes_remaining);
        }
    }
}


static gpointer
scan_thread (Scanner *scanner)
{
    SANE_Status status;
    SANE_Int version_code;
  
    g_hash_table_insert (scanners, g_thread_self (), scanner);
  
    scanner->priv->state = STATE_IDLE;

    status = sane_init (&version_code, authorization_cb);
    g_debug ("sane_init () -> %s", get_status_string (status));
    if (status != SANE_STATUS_GOOD) {
        g_warning ("Unable to initialize SANE backend: %s", sane_strstatus(status));
        return FALSE;
    }
    g_debug ("SANE version %d.%d.%d",
             SANE_VERSION_MAJOR(version_code),
             SANE_VERSION_MINOR(version_code),
             SANE_VERSION_BUILD(version_code));

    /* Scan for devices on first start */
    scanner_redetect (scanner);

    while (handle_requests (scanner)) {
        switch (scanner->priv->state) {
        case STATE_IDLE:
             if (scanner->priv->job_queue)
                 scanner->priv->state = STATE_OPEN;
             break;
        case STATE_REDETECT:
            do_redetect (scanner);
            break;
        case STATE_OPEN:
            do_open (scanner);
            break;
        case STATE_GET_OPTION:
            do_get_option (scanner);
            break;            
        case STATE_START:
            do_start (scanner);
            break;
        case STATE_GET_PARAMETERS:
            do_get_parameters (scanner);
            break;
        case STATE_READ:
            do_read (scanner);
            break;
        }
    }
    
    return NULL;
}


Scanner *
scanner_new ()
{
    return g_object_new (SCANNER_TYPE, NULL);
}


void
scanner_start (Scanner *scanner)
{
    GError *error = NULL;
    scanner->priv->thread = g_thread_create ((GThreadFunc) scan_thread, scanner, TRUE, &error);
    if (error) {
        g_critical ("Unable to create thread: %s", error->message);
        g_error_free (error);
    }    
}


void
scanner_redetect (Scanner *scanner)
{
    ScanRequest *request;

    if (scanner->priv->redetect)
        return;
    scanner->priv->redetect = TRUE;

    g_debug ("Requesting redetection of scan devices");

    request = g_malloc0 (sizeof (ScanRequest));
    request->type = REQUEST_REDETECT;
    g_async_queue_push (scanner->priv->scan_queue, request);
}


gboolean
scanner_is_scanning (Scanner *scanner)
{
    return scanner->priv->scanning;
}


void
scanner_scan (Scanner *scanner, const char *device,
              gint dpi, ScanMode scan_mode, gint depth, gboolean multi_page)
{
    ScanRequest *request;

    g_debug ("scanner_scan (\"%s\", %d, %s)", device ? device : "(null)", dpi, multi_page ? "TRUE" : "FALSE");
    request = g_malloc0 (sizeof (ScanRequest));
    request->type = REQUEST_START_SCAN;
    request->job = g_malloc0 (sizeof (ScanJob));
    request->job->device = g_strdup (device);
    request->job->dpi = dpi;
    request->job->scan_mode = scan_mode;
    request->job->depth = depth;
    request->job->multi_page = multi_page;
    g_async_queue_push (scanner->priv->scan_queue, request);
    scanner->priv->scanning = TRUE;
}


void
scanner_cancel (Scanner *scanner)
{
    ScanRequest *request;
  
    request = g_malloc0 (sizeof (ScanRequest));
    request->type = REQUEST_CANCEL;
    g_async_queue_push (scanner->priv->scan_queue, request);
}


void scanner_free (Scanner *scanner)
{
    ScanRequest *request;

    g_debug ("Stopping scan thread");

    request = g_malloc0 (sizeof (ScanRequest));
    request->type = REQUEST_QUIT;
    g_async_queue_push (scanner->priv->scan_queue, request);

    if (scanner->priv->thread)
        g_thread_join (scanner->priv->thread);

    g_async_queue_unref (scanner->priv->scan_queue);
    g_object_unref (scanner);

    sane_exit ();
    g_debug ("sane_exit ()");
}


static void
scanner_class_init (ScannerClass *klass)
{
    signals[AUTHORIZE] =
        g_signal_new ("authorize",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (ScannerClass, authorize),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__STRING,
                      G_TYPE_NONE, 1, G_TYPE_STRING);
    signals[UPDATE_DEVICES] =
        g_signal_new ("update-devices",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (ScannerClass, update_devices),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__POINTER,
                      G_TYPE_NONE, 1, G_TYPE_POINTER);
    signals[GOT_PAGE_INFO] =
        g_signal_new ("got-page-info",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (ScannerClass, got_page_info),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__POINTER,
                      G_TYPE_NONE, 1, G_TYPE_POINTER);
    signals[GOT_LINE] =
        g_signal_new ("got-line",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (ScannerClass, got_line),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__POINTER,
                      G_TYPE_NONE, 1, G_TYPE_POINTER);
    signals[SCAN_FAILED] =
        g_signal_new ("scan-failed",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (ScannerClass, scan_failed),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__POINTER,
                      G_TYPE_NONE, 1, G_TYPE_POINTER);
    signals[PAGE_DONE] =
        g_signal_new ("page-done",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (ScannerClass, page_done),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);
    signals[DOCUMENT_DONE] =
        g_signal_new ("document-done",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (ScannerClass, document_done),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);

    g_type_class_add_private (klass, sizeof (ScannerPrivate));

    scanners = g_hash_table_new (g_direct_hash, g_direct_equal);
}


static void
scanner_init (Scanner *scanner)
{
    scanner->priv = G_TYPE_INSTANCE_GET_PRIVATE (scanner, SCANNER_TYPE, ScannerPrivate);
    scanner->priv->scan_queue = g_async_queue_new ();
    scanner->priv->authorize_queue = g_async_queue_new ();
}
