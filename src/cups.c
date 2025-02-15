/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 * vim: set et ts=8 sw=8:
 *
 * Copyright (C) 2008 Novell, Inc.
 * Copyright (C) 2009 Red Hat, Inc.
 *
 * Authors: Vincent Untz, Tim Waugh
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include <pappl/device.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <grp.h>
#include <pwd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#define AVAHI_IF_UNSPEC -1
#define AVAHI_PROTO_INET 0
#define AVAHI_PROTO_INET6 1
#define AVAHI_PROTO_UNSPEC -1
#define AVAHI_BUS "org.freedesktop.Avahi"
#define AVAHI_SERVER_IFACE "org.freedesktop.Avahi.Server"
#define AVAHI_SERVICE_BROWSER_IFACE "org.freedesktop.Avahi.ServiceBrowser"
#define AVAHI_SERVICE_RESOLVER_IFACE "org.freedesktop.Avahi.ServiceResolver"

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include <cups/adminutil.h>
#include <cups/cups.h>
#include <cups/http.h>
#include <cups/ipp.h>
#include <cups/ppd.h>

#include "cups.h"

#include <pappl/pappl.h>

/* This is 0.1 second */
#define RECONNECT_DELAY        100000
/* We try to reconnect during 3 seconds. It's still a fairly long time even for
 * restarting cups, so it should be fine */
#define MAX_RECONNECT_ATTEMPTS 30

/*
     getPrinters
     getDests
     getClasses
     getPPDs
     getServerPPD
     getDocument
~!+* getDevices
     getJobs
     getJobAttributes
~!+* cancelJob
 !   cancelAllJobs
 !   authenticateJob
~!+* setJobHoldUntil
~!+* restartJob
~!+* getFile
~!+* putFile
~!+* addPrinter
~!+* setPrinterDevice
~!+* setPrinterInfo
~!+* setPrinterLocation
~!+* setPrinterShared
~!+* setPrinterJobSheets
~!+* setPrinterErrorPolicy
~!+* setPrinterOpPolicy
~!+* setPrinterUsersAllowed
~!+* setPrinterUsersDenied
~!+* addPrinterOptionDefault
~!+* deletePrinterOptionDefault
~!+* deletePrinter
     getPrinterAttributes
~!+* addPrinterToClass
~!+* deletePrinterFromClass
~!+* deleteClass
     getDefault
~!+* setDefault
     getPPD
~!+* enablePrinter
~!+* disablePrinter
~!+* acceptJobs
~!+* rejectJobs
     printTestPage
~!+* adminGetServerSettings
~!+* adminSetServerSettings
     getSubscriptions
     createSubscription
     getNotifications
     cancelSubscription
     renewSubscription
     printFile
     printFiles
*/
typedef void (printer_app_cb)(gpointer cb_data, gpointer user_data);

typedef enum
{
        CPH_RESOURCE_ROOT,
        CPH_RESOURCE_ADMIN,
        CPH_RESOURCE_JOBS
} CphResource;

G_DEFINE_TYPE (CphCups, cph_cups, G_TYPE_OBJECT)

#define CPH_CUPS_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), CPH_TYPE_CUPS, CphCupsPrivate))

struct CphCupsPrivate
{
        http_t       *connection;
        ipp_status_t  last_status;
        char         *internal_status;
};

static GObject *cph_cups_constructor (GType                  type,
                                      guint                  n_construct_properties,
                                      GObjectConstructParam *construct_properties);
static void     cph_cups_finalize    (GObject *object);

static void     _cph_cups_set_internal_status (CphCups    *cups,
                                               const char *status);


static void
cph_cups_class_init (CphCupsClass *klass)
{
        GObjectClass *object_class = G_OBJECT_CLASS (klass);

        object_class->constructor = cph_cups_constructor;
        object_class->finalize = cph_cups_finalize;

        g_type_class_add_private (klass, sizeof (CphCupsPrivate));
}

static GObject *
cph_cups_constructor (GType                  type,
                      guint                  n_construct_properties,
                      GObjectConstructParam *construct_properties)
{
        GObject *obj;
        CphCups *cups;

        obj = G_OBJECT_CLASS (cph_cups_parent_class)->constructor (
                                                type,
                                                n_construct_properties,
                                                construct_properties);

        cups = CPH_CUPS (obj);

        cups->priv->connection = httpConnectEncrypt (cupsServer (),
                                                     ippPort (),
                                                     cupsEncryption ());

        if (!cups->priv->connection) {
                g_critical ("Failed to connect to cupsd");
                g_object_unref (cups);
                return NULL;
        }

        return obj;
}

static void
cph_cups_init (CphCups *cups)
{
        cups->priv = CPH_CUPS_GET_PRIVATE (cups);

        cups->priv->connection = NULL;
        cups->priv->last_status = IPP_OK;
        cups->priv->internal_status = NULL;
}

static gboolean
cph_cups_reconnect (CphCups *cups)
{
        int return_value = -1;
        int i;

        for (i = 0; i < MAX_RECONNECT_ATTEMPTS; i++) {
                return_value = httpReconnect (cups->priv->connection);
                if (return_value == 0)
                        break;
                g_usleep (RECONNECT_DELAY);
        }

        if (return_value == 0)
                return TRUE;
        else
                return FALSE;
}

static void
cph_cups_finalize (GObject *object)
{
        CphCups *cups;

        g_return_if_fail (object != NULL);
        g_return_if_fail (CPH_IS_CUPS (object));

        cups = CPH_CUPS (object);

        if (cups->priv->connection)
                httpClose (cups->priv->connection);
        cups->priv->connection = NULL;

        if (cups->priv->internal_status)
                g_free (cups->priv->internal_status);
        cups->priv->internal_status = NULL;

        G_OBJECT_CLASS (cph_cups_parent_class)->finalize (object);
}

CphCups *
cph_cups_new (void)
{
        return g_object_new (CPH_TYPE_CUPS, NULL);
}

/******************************************************
 * Validation
 ******************************************************/

/* From https://bugzilla.novell.com/show_bug.cgi?id=447444#c5
 * We need to define a maximum length for strings to avoid cups
 * thinking there are multiple lines.
 */
#define CPH_STR_MAXLEN 512

#ifdef PATH_MAX
# define CPH_PATH_MAX PATH_MAX
#else
# define CPH_PATH_MAX 1024
#endif

static gboolean
_cph_cups_is_string_printable (const char *str,
                               gboolean    check_for_null,
                               gboolean    check_utf,
                               int         maxlen)
{
        int len;

        /* no NULL string */
        if (!str)
                return !check_for_null;

        len = strlen (str);
        if (maxlen > 0 && len > maxlen)
                return FALSE;

        if (check_utf) {
                const gchar *utf8_char;

                /* Check whether the string is valid UTF-8.
                 * This is what ippValidateAttribute() does for IPP_TAG_TEXT.
                 * See section 4.1.1 of RFC 2911. */
                if (!g_utf8_validate (str, -1, NULL))
                        return FALSE;

                /* only printable characters */
                for (utf8_char = str; *utf8_char != '\0'; utf8_char = g_utf8_next_char (utf8_char)) {
                        if (!g_unichar_isprint (g_utf8_get_char (utf8_char)))
                                return FALSE;
                }
        } else {
                int i;

                /* only printable characters */
                for (i = 0; i < len; i++) {
                        if (!g_ascii_isprint (str[i]))
                                return FALSE;
                }
        }

        return TRUE;
}

#define _CPH_CUPS_IS_VALID(name, name_for_str, check_for_null, check_utf, maxlen)    \
static gboolean                                                                      \
_cph_cups_is_##name##_valid (CphCups    *cups,                                       \
                             const char *str)                                        \
{                                                                                    \
        char *error;                                                                 \
                                                                                     \
        if (_cph_cups_is_string_printable (str, check_for_null, check_utf, maxlen))  \
                return TRUE;                                                         \
                                                                                     \
        error = g_strdup_printf ("\"%s\" is not a valid %s.",                        \
                                 str, name_for_str);                                 \
        _cph_cups_set_internal_status (cups, error);                                 \
        g_free (error);                                                              \
                                                                                     \
        return FALSE;                                                                \
}

static gboolean
_cph_cups_is_printer_name_valid_internal (const char *name)
{
        int i;
        int len;

        /* Quoting the lpadmin man page:
         *    CUPS allows printer names to contain any printable character
         *    except SPACE, TAB, "/", or  "#".
         * On top of that, validate_name() in lpadmin.c (from cups) checks that
         * the string is 127 characters long, or shorter. */

        /* no empty string */
        if (!name || name[0] == '\0')
                return FALSE;

        /* only printable strings with maximal length of 127 octets */
        if (!_cph_cups_is_string_printable (name, TRUE, TRUE, 127))
                return FALSE;

        /* no space, no /, no # */
        len = strlen (name);
        for (i = 0; i < len; i++) {
                if (g_ascii_isspace (name[i]))
                        return FALSE;
                if (name[i] == '/' || name[i] == '#')
                        return FALSE;
        }

        return TRUE;
}

static gboolean
_cph_cups_is_scheme_valid_internal (const char *scheme)
{
        int i;
        int len;

        /* no empty string */
        if (!scheme || scheme[0] == '\0')
                return FALSE;

        len = strlen (scheme);
        /* no string that is too long; see comment at the beginning of the
         * validation code block */
        if (len > CPH_STR_MAXLEN)
                return FALSE;

        /* From RFC 1738:
         * Scheme names consist of a sequence of characters. The lower case
         * letters "a"--"z", digits, and the characters plus ("+"), period
         * ("."), and hyphen ("-") are allowed. For resiliency, programs
         * interpreting URLs should treat upper case letters as equivalent to
         * lower case in scheme names (e.g., allow "HTTP" as well as "http").
         */
        for (i = 0; i < len; i++) {
                if (!g_ascii_isalnum (scheme[i]) &&
                    scheme[i] != '+' &&
                    scheme[i] != '.' &&
                    scheme[i] != '-')
                        return FALSE;
        }

        return TRUE;
}

static gboolean
_cph_cups_is_printer_name_valid (CphCups    *cups,
                                 const char *name)
{
        char *error;

        if (_cph_cups_is_printer_name_valid_internal (name))
                return TRUE;

        error = g_strdup_printf ("\"%s\" is not a valid printer name.", name);
        _cph_cups_set_internal_status (cups, error);
        g_free (error);

        return FALSE;
}

/* class is similar to printer in terms of validity checks */
static gboolean
_cph_cups_is_class_name_valid (CphCups    *cups,
                               const char *name)
{
        char *error;

        if (_cph_cups_is_printer_name_valid_internal (name))
                return TRUE;

        error = g_strdup_printf ("\"%s\" is not a valid class name.", name);
        _cph_cups_set_internal_status (cups, error);
        g_free (error);

        return FALSE;
}

static gboolean
_cph_cups_is_job_id_valid (CphCups *cups,
                           int      job_id)
{
        char *error;

        if (job_id > 0)
                return TRUE;

        error = g_strdup_printf ("\"%d\" is not a valid job id.", job_id);
        _cph_cups_set_internal_status (cups, error);
        g_free (error);

        return FALSE;
}

static gboolean
_cph_cups_is_scheme_valid (CphCups    *cups,
                           const char *scheme)
{
        char *error;

        if (_cph_cups_is_scheme_valid_internal (scheme))
                return TRUE;

        error = g_strdup_printf ("\"%s\" is not a valid scheme.", scheme);
        _cph_cups_set_internal_status (cups, error);
        g_free (error);

        return FALSE;
}

/* This is some text, but we could potentially do more checks. We don't do them
 * because cups will already do them.
 *   + for the URI, we could check that the scheme is supported and that the
 *     URI is a valid URI.
 *   + for the PPD, we could check that the PPD exists in the cups database.
 *     Another reason to not do this ourselves is that it's really slow to
 *     fetch all the PPDs.
 *   + for the PPD filename, we could check that the file exists and is a
 *     regular file (no socket, block device, etc.). It can be NULL for raw
 *     printers.
 *   + for the job sheet, we could check that the value is in the
 *     job-sheets-supported attribute.
 *   + for the policies, we could check that the value is in the
 *     printer-error-policy-supported and printer-op-policy-supported
 *     attributes.
 */
_CPH_CUPS_IS_VALID (printer_uri, "printer URI", TRUE, FALSE, CPH_STR_MAXLEN)
_CPH_CUPS_IS_VALID (ppd, "PPD", FALSE, FALSE, CPH_STR_MAXLEN)
_CPH_CUPS_IS_VALID (ppd_filename, "PPD file", FALSE, FALSE, CPH_STR_MAXLEN)
_CPH_CUPS_IS_VALID (job_sheet, "job sheet", FALSE, FALSE, CPH_STR_MAXLEN)
_CPH_CUPS_IS_VALID (error_policy, "error policy", FALSE, FALSE, CPH_STR_MAXLEN)
_CPH_CUPS_IS_VALID (op_policy, "op policy", FALSE, FALSE, CPH_STR_MAXLEN)

/* Check for users. Those are some printable strings, which souldn't be NULL.
 * They should also not be empty, but it appears that it's possible to carry
 * an empty "DenyUser" in the cups configuration, so we should handle (by
 * ignoring them) empty usernames.
 * We could also check that the username exists on the system, but cups will do
 * it.
 */
_CPH_CUPS_IS_VALID (user, "user", TRUE, FALSE, CPH_STR_MAXLEN)

/* Check for options & values. Those are for sure some printable strings, but
 * can we do more? Let's see:
 *   + an option seems to be, empirically, composed of alphanumerical
 *     characters, and dashes. However, this is not something we can be sure of
 *     and so we'll let cups handle that.
 *   + a value can be some text, and we don't know much more.
 */
_CPH_CUPS_IS_VALID (option, "option", TRUE, FALSE, CPH_STR_MAXLEN)
_CPH_CUPS_IS_VALID (option_value, "value for option", FALSE, FALSE, CPH_STR_MAXLEN)

/* This is really just some text */
_CPH_CUPS_IS_VALID (info, "description", FALSE, TRUE, CPH_STR_MAXLEN)
_CPH_CUPS_IS_VALID (location, "location", FALSE, TRUE, CPH_STR_MAXLEN)
_CPH_CUPS_IS_VALID (reject_jobs_reason, "reason", FALSE, TRUE, CPH_STR_MAXLEN)
_CPH_CUPS_IS_VALID (job_hold_until, "job hold until", FALSE, FALSE, CPH_STR_MAXLEN)

/* For put/get file: this is some text, but we could potentially do more
 * checks. We don't do them because cups will already do them.
 *   + for the resource, we could check that it starts with a /, for example.
 *   + for the filename, in the put case, we could check that the file exists
 *     and is a regular file (no socket, block device, etc.).
 */
_CPH_CUPS_IS_VALID (resource, "resource", TRUE, FALSE, CPH_STR_MAXLEN)
_CPH_CUPS_IS_VALID (filename, "filename", TRUE, FALSE, CPH_STR_MAXLEN)

/******************************************************
 * Helpers
 ******************************************************/

static gboolean
_cph_cups_set_effective_id (unsigned int   sender_uid,
                            int           *saved_ngroups,
                            gid_t        **saved_groups)
{
        struct passwd *password_entry;
        int            ngroups;
        gid_t         *groups;

        /* avoid g_assert() because we don't want to crash here */
        if (saved_ngroups == NULL || saved_groups == NULL) {
                g_critical ("Internal error: cannot save supplementary groups.");
                return FALSE;
        }

        *saved_ngroups = -1;
        *saved_groups = NULL;

        ngroups = getgroups (0, NULL);
        if (ngroups < 0)
                return FALSE;

        groups = g_new (gid_t, ngroups);
        if (groups == NULL && ngroups > 0)
                return FALSE;

        if (getgroups (ngroups, groups) < 0) {
                g_free (groups);

                return FALSE;
        }

        password_entry = getpwuid ((uid_t) sender_uid);

        if (password_entry == NULL ||
            setegid (password_entry->pw_gid) != 0) {
                g_free (groups);

                return FALSE;
        }

        if (initgroups (password_entry->pw_name,
                        password_entry->pw_gid) != 0) {
                if (getgid () != getegid ())
                        setegid (getgid ());

                g_free (groups);

                return FALSE;
        }


        if (seteuid (sender_uid) != 0) {
                if (getgid () != getegid ())
                        setegid (getgid ());

                setgroups (ngroups, groups);
                g_free (groups);

                return FALSE;
        }

        *saved_ngroups = ngroups;
        *saved_groups = groups;

        return TRUE;
}

static void
_cph_cups_reset_effective_id (int    saved_ngroups,
                              gid_t *saved_groups)
{
        seteuid (getuid ());
        setegid (getgid ());
        if (saved_ngroups >= 0)
                setgroups (saved_ngroups, saved_groups);
}

static void
_cph_cups_add_printer_uri (ipp_t      *request,
                           const char *name)
{
        char *escaped_name;
        char  uri[HTTP_MAX_URI + 1];

        escaped_name = g_uri_escape_string (name, NULL, FALSE);
        g_snprintf (uri, sizeof (uri),
                    "ipp://localhost/printers/%s", escaped_name);
        g_free (escaped_name);

        ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
                      "printer-uri", NULL, uri);
}

static void
_cph_cups_add_job_printer_uri (ipp_t      *request,
                               const char *name)
{
        char *escaped_name;
        char  uri[HTTP_MAX_URI + 1];

        escaped_name = g_uri_escape_string (name, NULL, FALSE);
        g_snprintf (uri, sizeof (uri),
                    "ipp://localhost/printers/%s", escaped_name);
        g_free (escaped_name);

        ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
                      "job-printer-uri", NULL, uri);
}

static void
_cph_cups_add_class_uri (ipp_t      *request,
                         const char *name)
{
        char *escaped_name;
        char  uri[HTTP_MAX_URI + 1];

        escaped_name = g_uri_escape_string (name, NULL, FALSE);
        g_snprintf (uri, sizeof (uri),
                    "ipp://localhost/classes/%s", escaped_name);
        g_free (escaped_name);

        ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
                      "printer-uri", NULL, uri);
}

static void
_cph_cups_add_job_uri (ipp_t      *request,
                       int         job_id)
{
        char uri[HTTP_MAX_URI + 1];

        g_snprintf (uri, sizeof (uri),
                    "ipp://localhost/jobs/%d", job_id);
        ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
                      "job-uri", NULL, uri);
}

static void
_cph_cups_add_requesting_user_name (ipp_t      *request,
                                    const char *username)
{
        if (username)
                ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
                              "requesting-user-name", NULL, username);
        else
                ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
                              "requesting-user-name", NULL, cupsUser ());
}

static void
_cph_cups_set_internal_status (CphCups    *cups,
                               const char *status)
{
        if (cups->priv->internal_status)
                g_free (cups->priv->internal_status);

        if (status)
                cups->priv->internal_status = g_strdup (status);
        else
                cups->priv->internal_status = NULL;
}

static void
_cph_cups_set_internal_status_from_http (CphCups       *cups,
                                         http_status_t  status)
{
        if (cups->priv->internal_status)
                g_free (cups->priv->internal_status);

        /* Only 2xx answers are okay */
        if (status < HTTP_OK ||
            status >= HTTP_MULTIPLE_CHOICES)
                cups->priv->internal_status = g_strdup (httpStatus (status));
        else
                cups->priv->internal_status = NULL;
}
static void
_cph_cups_set_error_from_reply (CphCups *cups,
                                ipp_t   *reply)
{
        if (reply)
                cups->priv->last_status = ippGetStatusCode (reply);
        else
                cups->priv->last_status = cupsLastError ();
}

static gboolean
_cph_cups_is_reply_ok (CphCups  *cups,
                       ipp_t    *reply,
                       gboolean  delete_reply_if_not_ok)
{
        /* reset the internal status: we'll use the cups status */
        _cph_cups_set_internal_status (cups, NULL);

        if (reply && ippGetStatusCode (reply) <= IPP_OK_CONFLICT) {
                cups->priv->last_status = IPP_OK;
                return TRUE;
        } else {
                _cph_cups_set_error_from_reply (cups, reply);
#if 0
                /* Useful when debugging: */
                g_print ("%s\n", cupsLastErrorString ());
#endif

                if (delete_reply_if_not_ok && reply)
                        ippDelete (reply);

                return FALSE;
        }
}

static gboolean
_cph_cups_handle_reply (CphCups *cups,
                        ipp_t   *reply)
{
        gboolean retval;

        retval = _cph_cups_is_reply_ok (cups, reply, FALSE);

        if (reply)
                ippDelete (reply);

        return retval;
}

static const char *
_cph_cups_get_resource (CphResource resource)
{
        switch (resource) {
                case CPH_RESOURCE_ROOT:
                        return "/";
                case CPH_RESOURCE_ADMIN:
                        return "/admin/";
                case CPH_RESOURCE_JOBS:
                        return "/jobs/";
                default:
                        /* that's a fallback -- we don't use
                         * g_assert_not_reached() to avoir crashing. */
                        g_critical ("Asking for a resource with no match.");
                        return "/";
        }
}

static gboolean
_cph_cups_send_request (CphCups     *cups,
                        ipp_t       *request,
                        CphResource  resource)
{
        ipp_t      *reply;
        const char *resource_char;

        resource_char = _cph_cups_get_resource (resource);
        reply = cupsDoRequest (cups->priv->connection, request, resource_char);

        return _cph_cups_handle_reply (cups, reply);
}

static gboolean
_cph_cups_post_request (CphCups     *cups,
                        ipp_t       *request,
                        const char  *file,
                        CphResource  resource)
{
        ipp_t *reply;
        const char *resource_char;

        resource_char = _cph_cups_get_resource (resource);

        if (file && file[0] != '\0')
                reply = cupsDoFileRequest (cups->priv->connection, request,
                                           resource_char, file);
        else
                reply = cupsDoFileRequest (cups->priv->connection, request,
                                           resource_char, NULL);

        return _cph_cups_handle_reply (cups, reply);
}

static gboolean
_cph_cups_send_new_simple_request (CphCups     *cups,
                                   ipp_op_t     op,
                                   const char  *printer_name,
                                   CphResource  resource)
{
        ipp_t *request;

        if (!_cph_cups_is_printer_name_valid (cups, printer_name))
                return FALSE;

        request = ippNewRequest (op);
        _cph_cups_add_printer_uri (request, printer_name);
        _cph_cups_add_requesting_user_name (request, NULL);

        return _cph_cups_send_request (cups, request, resource);
}

static gboolean
_cph_cups_send_new_simple_class_request (CphCups     *cups,
                                         ipp_op_t     op,
                                         const char  *class_name,
                                         CphResource  resource)
{
        ipp_t *request;

        if (!_cph_cups_is_class_name_valid (cups, class_name))
                return FALSE;

        request = ippNewRequest (op);
        _cph_cups_add_class_uri (request, class_name);
        _cph_cups_add_requesting_user_name (request, NULL);

        return _cph_cups_send_request (cups, request, resource);
}

static gboolean
_cph_cups_send_new_printer_class_request (CphCups     *cups,
                                          const char  *printer_name,
                                          ipp_tag_t    group,
                                          ipp_tag_t    type,
                                          const char  *name,
                                          const char  *value)
{
        ipp_t *request;

        request = ippNewRequest (CUPS_ADD_MODIFY_PRINTER);
        _cph_cups_add_printer_uri (request, printer_name);
        _cph_cups_add_requesting_user_name (request, NULL);
        ippAddString (request, group, type, name, NULL, value);

        if (_cph_cups_send_request (cups, request, CPH_RESOURCE_ADMIN))
                return TRUE;

        /* it failed, maybe it was a class? */
        if (cups->priv->last_status != IPP_NOT_POSSIBLE)
                return FALSE;

        request = ippNewRequest (CUPS_ADD_MODIFY_CLASS);
        _cph_cups_add_class_uri (request, printer_name);
        _cph_cups_add_requesting_user_name (request, NULL);
        ippAddString (request, group, type, name, NULL, value);

        return _cph_cups_send_request (cups, request, CPH_RESOURCE_ADMIN);
}

static gboolean
_cph_cups_send_new_simple_job_request (CphCups     *cups,
                                       ipp_op_t     op,
                                       int          job_id,
                                       const char  *user_name,
                                       CphResource  resource)
{
        ipp_t *request;

        request = ippNewRequest (op);
        _cph_cups_add_job_uri (request, job_id);

        if (user_name != NULL)
                _cph_cups_add_requesting_user_name (request, user_name);

        return _cph_cups_send_request (cups, request, resource);
}

static gboolean
_cph_cups_send_new_job_attributes_request (CphCups     *cups,
                                           int          job_id,
                                           const char  *name,
                                           const char  *value,
                                           const char  *user_name,
                                           CphResource  resource)
{
        cups_option_t *options = NULL;
        ipp_t         *request;
        int            num_options = 0;

        request = ippNewRequest (IPP_SET_JOB_ATTRIBUTES);
        _cph_cups_add_job_uri (request, job_id);

        if (user_name != NULL)
                _cph_cups_add_requesting_user_name (request, user_name);

        num_options = cupsAddOption (name, value,
                                     num_options, &options);
        cupsEncodeOptions (request, num_options, options);

        return _cph_cups_send_request (cups, request, resource);
}

static const char *
_cph_cups_get_attribute_string (ipp_t           *reply,
                                ipp_tag_t        group,
                                const char      *name,
                                ipp_tag_t        type)
{
        ipp_attribute_t *attr;

        for (attr = ippFirstAttribute (reply); attr; attr = ippNextAttribute (reply)) {
                while (attr && ippGetGroupTag (attr) != group)
                        attr = ippNextAttribute (reply);

                if (attr == NULL)
                        break;

                while (attr && ippGetGroupTag (attr) == group) {
                        if (ippGetName (attr) &&
                            strcmp (ippGetName (attr), name) == 0 &&
                            ippGetValueTag (attr) == type) {
                                return ippGetString (attr, 0, NULL);
                        }

                        attr = ippNextAttribute (reply);
                }

                if (attr == NULL)
                        break;
        }

        return NULL;
}

static int
_cph_cups_class_has_printer (CphCups     *cups,
                             const char  *class_name,
                             const char  *printer_name,
                             ipp_t      **reply)
{
        gboolean         retval;
        const char      *resource_char;
        ipp_t           *request;
        ipp_t           *internal_reply;
        ipp_attribute_t *printer_names;
        int              i;

        retval = -1;

        if (reply)
                *reply = NULL;

        request = ippNewRequest (IPP_GET_PRINTER_ATTRIBUTES);
        _cph_cups_add_class_uri (request, class_name);
        _cph_cups_add_requesting_user_name (request, NULL);
        resource_char = _cph_cups_get_resource (CPH_RESOURCE_ROOT);
        internal_reply = cupsDoRequest (cups->priv->connection,
                                        request, resource_char);

        if (!internal_reply)
                return -1;

        printer_names = ippFindAttribute (internal_reply,
                                          "member-names", IPP_TAG_NAME);

        if (!printer_names)
                goto out;

        for (i = 0; i < ippGetCount (printer_names); i++) {
                if (!g_ascii_strcasecmp (ippGetString (printer_names, i, NULL),
                                         printer_name)) {
                        retval = i;
                        break;
                }
        }

out:
        if (reply)
                *reply = internal_reply;
        else
                ippDelete (internal_reply);

        return retval;
}

static gboolean
_cph_cups_printer_class_set_users (CphCups           *cups,
                                   const char        *printer_name,
                                   const char *const *users,
                                   const char        *request_name,
                                   const char        *default_value)
{
        int              real_len;
        int              len;
        ipp_t           *request;
        ipp_attribute_t *attr;

        real_len = 0;
        len = 0;
        if (users) {
                while (users[real_len] != NULL) {
                        if (users[real_len][0] != '\0')
                                len++;
                        real_len++;
                }
        }

        request = ippNewRequest (CUPS_ADD_MODIFY_PRINTER);
        _cph_cups_add_printer_uri (request, printer_name);
        _cph_cups_add_requesting_user_name (request, NULL);
        attr = ippAddStrings (request, IPP_TAG_PRINTER, IPP_TAG_NAME,
                              request_name, len ? len : 1, NULL, NULL);
        if (len == 0)
                ippSetString (request, &attr, 0, g_strdup (default_value));
        else {
                int i, j;
                for (i = 0, j = 0; i < real_len && j < len; i++) {
                        /* we skip empty user names */
                        if (users[i][0] == '\0')
                                continue;

                        ippSetString (request, &attr, j, g_strdup (users[i]));
                        j++;
                }
        }

        if (_cph_cups_send_request (cups, request, CPH_RESOURCE_ADMIN))
                return TRUE;

        /* it failed, maybe it was a class? */
        if (cups->priv->last_status != IPP_NOT_POSSIBLE)
                return FALSE;

        request = ippNewRequest (CUPS_ADD_MODIFY_CLASS);
        _cph_cups_add_class_uri (request, printer_name);
        _cph_cups_add_requesting_user_name (request, NULL);
        attr = ippAddStrings (request, IPP_TAG_PRINTER, IPP_TAG_NAME,
                              request_name, len ? len : 1, NULL, NULL);
        if (len == 0)
                ippSetString (request, &attr, 0, g_strdup (default_value));
        else {
                int i, j;
                for (i = 0, j = 0; i < real_len && j < len; i++) {
                        /* we skip empty user names */
                        if (users[i][0] == '\0')
                                continue;

                        ippSetString (request, &attr, j, g_strdup (users[i]));
                        j++;
                }
        }

        return _cph_cups_send_request (cups, request, CPH_RESOURCE_ADMIN);
}

/******************************************************
 * Now, the real methods
 ******************************************************/

const char *
cph_cups_last_status_to_string (CphCups *cups)
{
        g_return_val_if_fail (CPH_IS_CUPS (cups), "");

        if (cups->priv->internal_status)
                return cups->priv->internal_status;
        else
                return ippErrorString (cups->priv->last_status);
}

gboolean
cph_cups_is_class (CphCups    *cups,
                   const char *name)
{
        const char * const  attrs[1] = { "member-names" };
        ipp_t              *request;
        const char         *resource_char;
        ipp_t              *reply;
        gboolean            retval;

        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        if (!_cph_cups_is_class_name_valid (cups, name))
                return FALSE;

        request = ippNewRequest (IPP_GET_PRINTER_ATTRIBUTES);
        _cph_cups_add_class_uri (request, name);
        _cph_cups_add_requesting_user_name (request, NULL);
        ippAddStrings (request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
                       "requested-attributes", 1, NULL, attrs);

        resource_char = _cph_cups_get_resource (CPH_RESOURCE_ROOT);
        reply = cupsDoRequest (cups->priv->connection,
                               request, resource_char);

        if (!_cph_cups_is_reply_ok (cups, reply, TRUE))
                return FALSE;

        /* Note: we need to look if the attribute is there, since we get a
         * reply if the name is a printer name and not a class name. The
         * attribute is the only way to distinguish the two cases. */
        retval = ippFindAttribute (reply, attrs[0], IPP_TAG_NAME) != NULL;

        if (reply)
                ippDelete (reply);

        return retval;
}

char *
cph_cups_printer_get_uri (CphCups    *cups,
                          const char *printer_name)
{
        const char * const  attrs[1] = { "device-uri" };
        ipp_t              *request;
        const char         *resource_char;
        ipp_t              *reply;
        const char         *const_uri;
        char               *uri;

        g_return_val_if_fail (CPH_IS_CUPS (cups), NULL);

        if (!_cph_cups_is_printer_name_valid (cups, printer_name))
                return NULL;

        request = ippNewRequest (IPP_GET_PRINTER_ATTRIBUTES);
        _cph_cups_add_printer_uri (request, printer_name);
        _cph_cups_add_requesting_user_name (request, NULL);
        ippAddStrings (request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
                       "requested-attributes", 1, NULL, attrs);

        resource_char = _cph_cups_get_resource (CPH_RESOURCE_ROOT);
        reply = cupsDoRequest (cups->priv->connection,
                               request, resource_char);

        if (!_cph_cups_is_reply_ok (cups, reply, TRUE))
                return NULL;

        const_uri = _cph_cups_get_attribute_string (reply, IPP_TAG_PRINTER,
                                                    attrs[0], IPP_TAG_URI);

        uri = NULL;

        if (const_uri)
                uri = g_strdup (const_uri);

        ippDelete (reply);

        return uri;
}

gboolean
cph_cups_is_printer_local (CphCups    *cups,
                           const char *printer_name)
{
        char     *uri;
        gboolean  retval;

        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        if (!_cph_cups_is_printer_name_valid (cups, printer_name))
                return FALSE;

        uri = cph_cups_printer_get_uri (cups, printer_name);

        /* This can happen, especially since the printer might not exist, or if
         * it's actually a class and not a printer. In all cases, it should be
         * considered local. */
        if (!uri)
                return TRUE;

        retval = cph_cups_is_printer_uri_local (uri);

        g_free (uri);

        return retval;
}

gboolean
cph_cups_file_get (CphCups      *cups,
                   const char   *resource,
                   const char   *filename,
                   unsigned int  sender_uid)
{
        int           saved_ngroups = -1;
        gid_t        *saved_groups = NULL;
        http_status_t status;
        int           fd;
        struct stat   file_stat;
        char         *error;

        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        if (!_cph_cups_is_resource_valid (cups, resource))
                return FALSE;
        if (!_cph_cups_is_filename_valid (cups, filename))
                return FALSE;

        if (!_cph_cups_set_effective_id (sender_uid,
                                         &saved_ngroups, &saved_groups)) {
                error = g_strdup_printf ("Cannot check if \"%s\" is "
                                         "writable: %s",
                                         filename, strerror (errno));
                _cph_cups_set_internal_status (cups, error);
                g_free (error);

                return FALSE;
        }

        fd = open (filename, O_WRONLY | O_NOFOLLOW | O_TRUNC);

        _cph_cups_reset_effective_id (saved_ngroups, saved_groups);
        g_free (saved_groups);

        if (fd < 0) {
                error = g_strdup_printf ("Cannot open \"%s\": %s",
                                         filename, strerror (errno));
                _cph_cups_set_internal_status (cups, error);
                g_free (error);

                return FALSE;
        }


        if (fstat (fd, &file_stat) != 0) {
                error = g_strdup_printf ("Cannot write to \"%s\": %s",
                                         filename, strerror (errno));
                _cph_cups_set_internal_status (cups, error);
                g_free (error);

                close (fd);

                return FALSE;
        }

        if (!S_ISREG (file_stat.st_mode)) {
                /* hrm, this looks suspicious... we won't help */
                error = g_strdup_printf ("File \"%s\" is not a regular file.",
                                         filename);
                _cph_cups_set_internal_status (cups, error);
                g_free (error);

                close (fd);

                return FALSE;
        }

        /* reset the internal status: we'll use the http status */
        _cph_cups_set_internal_status (cups, NULL);

        status = cupsGetFd (cups->priv->connection, resource, fd);

        /* FIXME: There's a bug where the cups connection can fail with EPIPE.
         * We're working around it here until it's fixed in cups. */
        if (status != HTTP_OK) {
                if (cph_cups_reconnect (cups))
                        status = cupsGetFd (cups->priv->connection,
                                            resource, fd);
        }

        close (fd);

        _cph_cups_set_internal_status_from_http (cups, status);

        return (status == HTTP_OK);
}

gboolean
cph_cups_file_put (CphCups      *cups,
                   const char   *resource,
                   const char   *filename,
                   unsigned int  sender_uid)
{
        int           saved_ngroups = -1;
        gid_t        *saved_groups = NULL;
        http_status_t status;
        int           fd;
        struct stat   file_stat;
        char         *error;

        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        if (!_cph_cups_is_resource_valid (cups, resource))
                return FALSE;
        if (!_cph_cups_is_filename_valid (cups, filename))
                return FALSE;

        if (!_cph_cups_set_effective_id (sender_uid,
                                         &saved_ngroups, &saved_groups)) {
                error = g_strdup_printf ("Cannot check if \"%s\" is "
                                         "readable: %s",
                                         filename, strerror (errno));
                _cph_cups_set_internal_status (cups, error);
                g_free (error);

                return FALSE;
        }

        fd = open (filename, O_RDONLY);

        _cph_cups_reset_effective_id (saved_ngroups, saved_groups);
        g_free (saved_groups);

        if (fd < 0) {
                error = g_strdup_printf ("Cannot open \"%s\": %s",
                                         filename, strerror (errno));
                _cph_cups_set_internal_status (cups, error);
                g_free (error);

                return FALSE;
        }

        if (fstat (fd, &file_stat) != 0) {
                error = g_strdup_printf ("Cannot read \"%s\": %s",
                                         filename, strerror (errno));
                _cph_cups_set_internal_status (cups, error);
                g_free (error);

                close (fd);

                return FALSE;
        }

        if (!S_ISREG (file_stat.st_mode)) {
                /* hrm, this looks suspicious... we won't help */
                error = g_strdup_printf ("File \"%s\" is not a regular file.",
                                         filename);
                _cph_cups_set_internal_status (cups, error);
                g_free (error);

                close (fd);

                return FALSE;
        }

        /* reset the internal status: we'll use the http status */
        _cph_cups_set_internal_status (cups, NULL);

        status = cupsPutFd (cups->priv->connection, resource, fd);

        close (fd);

        _cph_cups_set_internal_status_from_http (cups, status);

        /* CUPS is being restarted, so we need to reconnect */
        cph_cups_reconnect (cups);

        return (status == HTTP_OK ||
                status == HTTP_CREATED);
}

/* Functions that are for the server in general */

gboolean
cph_cups_server_get_settings (CphCups   *cups,
                              GVariant **settings)
{
        int              retval;
        GVariantBuilder *builder;
        cups_option_t   *cups_settings;
        int              num_settings, i;

        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);
        g_return_val_if_fail (settings != NULL, FALSE);

        *settings = NULL;

        retval = cupsAdminGetServerSettings (cups->priv->connection,
                                             &num_settings, &cups_settings);

        if (retval == 0) {
                _cph_cups_set_internal_status (cups,
                                               "Cannot get server settings.");

                return FALSE;
        }

        builder = g_variant_builder_new (G_VARIANT_TYPE ("a{ss}"));

        for (i = 0; i < num_settings; i++)
                g_variant_builder_add (builder, "{ss}",
                                       cups_settings[i].name,
                                       cups_settings[i].value);

        cupsFreeOptions (num_settings, cups_settings);

        *settings = g_variant_builder_end (builder);

        g_variant_builder_unref (builder);

        return TRUE;
}

gboolean
cph_cups_server_set_settings (CphCups  *cups,
                              GVariant *settings)
{
        int             retval;
        GVariantIter   *iter;
        /* key and value are strings, but we want to avoid compiler warnings */
        gpointer        key;
        gpointer        value;
        cups_option_t  *cups_settings;
        int             num_settings;

        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);
        g_return_val_if_fail (settings != NULL, FALSE);

        /* First pass to check the validity of the hashtable content */
        g_variant_get (settings, "a{ss}", &iter);
        while (g_variant_iter_loop (iter, "{ss}", &key, &value)) {
                if (!_cph_cups_is_option_valid (cups, key))
                        return FALSE;
                if (!_cph_cups_is_option_value_valid (cups, value))
                        return FALSE;
        }
        g_variant_iter_free (iter);

        /* Second pass to actually set the settings */
        cups_settings = NULL;
        num_settings = 0;

        g_variant_get (settings, "a{ss}", &iter);
        while (g_variant_iter_loop (iter, "{ss}", &key, &value))
                num_settings = cupsAddOption (key, value,
                                              num_settings, &cups_settings);
        g_variant_iter_free (iter);

        retval = cupsAdminSetServerSettings (cups->priv->connection,
                                             num_settings, cups_settings);

        /* CUPS is being restarted, so we need to reconnect */
        cph_cups_reconnect (cups);

        cupsFreeOptions (num_settings, cups_settings);

        if (retval == 0) {
                _cph_cups_set_internal_status (cups,
                                               "Cannot set server settings.");

                return FALSE;
        }

        return TRUE;
}

typedef struct {
        int              iter;
        int              limit;
        GVariantBuilder *builder;
} CphCupsGetDevices;

typedef struct {
        int              iter;
        GVariantBuilder *builder;
} CphCupsGetPrinterApps;

static void
_cph_cups_get_devices_cb (const char *device_class,
                          const char *device_id,
                          const char *device_info,
                          const char *device_make_and_model,
                          const char *device_uri,
                          const char *device_location,
                          void       *user_data)
{
        CphCupsGetDevices *data = user_data;
        char              *key;

        g_return_if_fail (data != NULL);

        if (data->limit > 0 && data->iter >= data->limit)
                return;

        if (device_class && device_class[0] != '\0') {
                key  = g_strdup_printf ("device-class:%d", data->iter);
                g_variant_builder_add (data->builder, "{ss}",
                                       key, device_class);
                g_free (key);
        }
        if (device_id && device_id[0] != '\0') {
                key  = g_strdup_printf ("device-id:%d", data->iter);
                g_variant_builder_add (data->builder, "{ss}",
                                       key, device_id);
                g_free (key);
        }
        if (device_info && device_info[0] != '\0') {
                key  = g_strdup_printf ("device-info:%d", data->iter);
                g_variant_builder_add (data->builder, "{ss}",
                                       key, device_info);
                g_free (key);
        }
        if (device_make_and_model && device_make_and_model[0] != '\0') {
                key  = g_strdup_printf ("device-make-and-model:%d", data->iter);
                g_variant_builder_add (data->builder, "{ss}",
                                       key, device_make_and_model);
                g_free (key);
        }
        if (device_uri && device_uri[0] != '\0') {
                key  = g_strdup_printf ("device-uri:%d", data->iter);
                g_variant_builder_add (data->builder, "{ss}",
                                       key, device_uri);
                g_free (key);
        }
        if (device_location && device_location[0] != '\0') {
                key  = g_strdup_printf ("device-location:%d", data->iter);
                g_variant_builder_add (data->builder, "{ss}",
                                       key, device_location);
                g_free (key);
        }
        data->iter++;
}

typedef struct
{

	ipp_t *response;
	gchar *buff;
	int buff_size;
        cups_dest_t* service;

} add_attribute_data;

typedef struct 
{
        char                *avahi_service_browser_path;
        guint                avahi_service_browser_subscription_id;
        guint                avahi_service_browser_subscription_id_ind;           
        guint                unsubscribe_general_subscription_id;
        GDBusConnection     *dbus_connection;
        GCancellable        *avahi_cancellable;
        GList               *system_objects;
        GMainLoop           *loop;
        gpointer             user_data;
        char                *service_type;
        CphCups             *cups;
        printer_app_cb      *callback;
        void                *data;
        gboolean             done;
} Avahi;

typedef struct
{
        GList                *services;
        gchar                *location;
        gchar                *address;
        gchar                *hostname;
        gchar                *name;
        gchar                *resource_path;
        gchar                *type;
        gchar                *domain;
        gchar                *UUID;
        gchar                *object_type;
        gchar                *admin_url;
        gchar                *uri;
        gchar                *objAttr;
        gint64               printer_type,
                             printer_state;
        gboolean             got_printer_state,
                             got_printer_type;
        int                  port;
        int                  family;
        gpointer             user_data;
} AvahiData;

typedef struct
{
        CphCupsGetDevices *data;
        gint              signal_pappl;      
}
pappl_data_callback;

int
pappl_timeout_cb (gpointer      user_data)
{
        ((pappl_data_callback*)(user_data))->signal_pappl = 1;

        return 0;
}

int compare_services (gconstpointer      data1,
                      gconstpointer      data2)
{
        AvahiData *data_1 = (AvahiData*)data1;
        AvahiData *data_2 = (AvahiData*)data2;
        
        return g_strcmp0 (data_1->name,data_2->name);
}

static gboolean
avahi_txt_get_key_value_pair (const gchar  *entry,
                              gchar       **key,
                              gchar       **value)
{
  const gchar *equal_sign;

  *key = NULL;
  *value = NULL;

  if (entry != NULL)
    {
      equal_sign = strstr (entry, "=");

      if (equal_sign != NULL)
        {
          *key = g_strndup (entry, equal_sign - entry);
          *value = g_strdup (equal_sign + 1);

          return TRUE;
        }
    }

  return FALSE;
}

static void
avahi_service_resolver_cb (GVariant*     output,
                           gpointer      user_data)
{
        AvahiData               *data;
        Avahi                   *backend;
        const char              *name;
        const char              *hostname;
        const char              *type;
        const char              *domain;
        const char              *address;
        char                    *key;
        char                    *value;
        char                    *tmp;
        char                    *endptr;
        GVariant                *txt,
                                *child;
        guint32                  flags;
        guint16                  port;
        GError                  *error = NULL;
        GList                   *iter;
        gsize                    length; 
        int                      interface;
        int                      protocol;
        int                      aprotocol;
        int                      i;


        backend = user_data;
        if (output)
        {

                g_variant_get (output, "(ii&s&s&s&si&sq@aayu)",
                               &interface,
                               &protocol,
                               &name,
                               &type,
                               &domain,
                               &hostname,
                               &aprotocol,
                               &address,
                               &port,
                               &txt,
                               &flags);

                data = g_new0 (AvahiData, 1);
                data->user_data = backend->user_data;
                
                if (g_strcmp0 (type, "_ipps-system._tcp") == 0 ||
                    g_strcmp0 (type, "_ipp-system._tcp") == 0)
                  {
                       data->object_type = g_strdup("SYSTEM_OBJECT");
                  } 
                else
                  {
                      data->object_type = g_strdup("PRINTER_OBJECT");
                  }

            for (i = 0; i < g_variant_n_children (txt); i++)
            {
              child = g_variant_get_child_value (txt, i);

              length = g_variant_get_size (child);
              if (length > 0)
                {
                  tmp = g_strndup (g_variant_get_data (child), length);
                  g_variant_unref (child);

                  if (!avahi_txt_get_key_value_pair (tmp, &key, &value))
                    {
                      g_free (tmp);
                      continue;
                    }

                  if (g_strcmp0 (key, "rp") == 0)
                    {
                      data->resource_path = g_strdup (value);
                    }
                  else if (g_strcmp0 (key, "note") == 0)
                    {
                      data->location = g_strdup (value);
                    }
                  else if (g_strcmp0 (key, "printer-type") == 0)
                    {
                      endptr = NULL;
                      data->printer_type = g_ascii_strtoull (value, &endptr, 16);
                      if (data->printer_type != 0 || endptr != value)
                        data->got_printer_type = TRUE;
                    }
                  else if (g_strcmp0 (key, "printer-state") == 0)
                    {
                      endptr = NULL;
                      data->printer_state = g_ascii_strtoull (value, &endptr, 10);
                      if (data->printer_state != 0 || endptr != value)
                        data->got_printer_state = TRUE;
                    }
                  else if (g_strcmp0 (key, "UUID") == 0)
                    {
                      if (*value != '\0')
                        data->UUID = g_strdup (value);
                    }
                  else if (g_strcmp0 (key, "adminurl") == 0)
                    {
                      if (*value != '\0')
                        data->admin_url = g_strdup (value);
                    }
                  g_clear_pointer (&key, g_free);
                  g_clear_pointer (&value, g_free);
                  g_free (tmp);
                }
              else
                {
                  g_variant_unref (child);
                }
            }

                data->address = g_strdup (address);
                data->hostname = g_strdup (hostname);
                data->port = port;
                data->family = protocol;
                data->name = g_strdup (name);
                data->type = g_strdup (type);
                data->domain = g_strdup (domain);
                data->services = NULL;
                
                g_variant_unref (txt);
                g_variant_unref (output);

                iter = g_list_find_custom (backend->system_objects, (gconstpointer) data, (GCompareFunc) compare_services);
                if (iter == NULL)
                  {
                     backend->system_objects = g_list_append (backend->system_objects, data);
                    //  if (g_strcmp0(data->object_type, "SYSTEM_OBJECT") == 0)
                    //   get_services (data);
                    //  else    Check new method for getting device from IPP request
                //       add_device (data);    // Callbacks
                        backend->callback (data, backend);
                        g_message("%s\n", data->hostname);

                  }
                else 
                 {
                     g_free (data->location);
                     g_free (data->address);
                     g_free (data->hostname);
                     g_free (data->name);
                     g_free (data->resource_path);
                     g_free (data->type);
                     g_free (data->domain);
                     g_free (data);
                 }
         }   
        else
         {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                  {
                        char *message = g_strdup_printf ("%s", error->message); 
                        // _cph_cups_set_internal_status ( backend->cups, message );
                        g_free (message);
                  }
                g_error_free (error);
         }

        return;
}

static gboolean
unsubscribe_general_subscription_cb (gpointer user_data)
{
        Avahi *printer_app_backend = user_data;

        g_dbus_connection_signal_unsubscribe (printer_app_backend->dbus_connection,
                                              printer_app_backend->avahi_service_browser_subscription_id);
        printer_app_backend->avahi_service_browser_subscription_id = 0;
        printer_app_backend->unsubscribe_general_subscription_id = 0;

        return G_SOURCE_REMOVE;
}

static void
avahi_service_browser_signal_handler (GDBusConnection *connection,
                                      const char      *sender_name,
                                      const char      *object_path,
                                      const char      *interface_name,
                                      const char      *signal_name,
                                      GVariant        *parameters,
                                      gpointer         user_data)
{
        Avahi               *backend;
        char                *name;
        char                *type;
        char                *domain;
        guint                flags;
        int                  interface;
        int                  protocol;

        backend = user_data;  

        if (g_strcmp0 (signal_name, "ItemNew") == 0)
          {
            g_variant_get (parameters, "(ii&s&s&su)",
                           &interface,
                           &protocol,
                           &name,
                           &type,
                           &domain,
                           &flags);

        GVariant *output = g_dbus_connection_call_sync (backend->dbus_connection,
                        AVAHI_BUS,
                        "/",
                        AVAHI_SERVER_IFACE,
                        "ResolveService",
                        g_variant_new ("(iisssiu)",
                                       interface,
                                       protocol,
                                       name,
                                       type,
                                       domain,
                                       AVAHI_PROTO_UNSPEC,
                                       0),
                        G_VARIANT_TYPE ("(iissssisqaayu)"),
                        G_DBUS_CALL_FLAGS_NONE,
                        -1,
                        backend->avahi_cancellable,
                        NULL);

        avahi_service_resolver_cb(output, user_data);
              
          }
        else if (g_strcmp0 (signal_name, "ItemRemove") == 0)
          {
            g_variant_get (parameters, "(ii&s&s&su)",
                           &interface,
                           &protocol,
                           &name,
                           &type,
                           &domain,
                           &flags);


                 GList *iter = g_list_find_custom (backend->system_objects, name , (GCompareFunc) compare_services);
                if (iter != NULL)
                  {
                    backend->system_objects = g_list_delete_link (backend->system_objects, iter);
                    g_free (iter->data);
                  }

          }
        else if (g_strcmp0 (signal_name, "AllForNow"))
          {
        //     backend->done = TRUE;
                g_main_loop_quit(backend->loop);
          }

   return;
}

static void
avahi_service_browser_new_cb (GVariant*          output,
                              gpointer           user_data)
{
        Avahi               *printer_device_backend;
        GError              *error = NULL;
        printer_device_backend = user_data;

        if (output)
          {

            g_variant_get (output, "(o)", &printer_device_backend->avahi_service_browser_path);
            printer_device_backend->avahi_service_browser_subscription_id =
              g_dbus_connection_signal_subscribe (printer_device_backend->dbus_connection,
                                                  NULL,
                                                  AVAHI_SERVICE_BROWSER_IFACE,
                                                  NULL,
                                                  printer_device_backend->avahi_service_browser_path,
                                                  NULL,
                                                  G_DBUS_SIGNAL_FLAGS_NONE,
                                                  avahi_service_browser_signal_handler,
                                                  printer_device_backend,
                                                  NULL);

            // if (printer_device_backend->avahi_service_browser_path)
            //     printer_device_backend->unsubscribe_general_subscription_id = g_idle_add (unsubscribe_general_subscription_cb, printer_device_backend);

            g_variant_unref (output);
          }
        else
          {
            /*
             * The creation of ServiceBrowser fails with G_IO_ERROR_DBUS_ERROR
             * if Avahi is disabled.
             */
            if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_DBUS_ERROR) &&
                !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                //  _cph_cups_set_internal_status(printer_device_backend->cups, g_strdup_printf("%s", error->message));
            g_error_free (error);
          }
        
}

static void
avahi_create_browsers (gpointer    user_data)
{ 
        Avahi               *backend;  
        backend = user_data;

        backend->avahi_service_browser_subscription_id =
          g_dbus_connection_signal_subscribe  (backend->dbus_connection,
                                               NULL,
                                               AVAHI_SERVICE_BROWSER_IFACE,
                                               NULL,
                                               NULL,
                                               NULL,
                                               G_DBUS_SIGNAL_FLAGS_NONE,
                                               avahi_service_browser_signal_handler,
                                               backend,
                                               NULL);

         GVariant* output = g_dbus_connection_call_sync (backend->dbus_connection,
                                AVAHI_BUS,
                                "/",
                                AVAHI_SERVER_IFACE,
                                "ServiceBrowserNew",
                                g_variant_new ("(iissu)",
                                               AVAHI_IF_UNSPEC,
                                               AVAHI_PROTO_UNSPEC,
                                               backend->service_type,
                                               "",
                                               0),
                                G_VARIANT_TYPE ("(o)"),
                                G_DBUS_CALL_FLAGS_NONE,
                                -1,
                                backend->avahi_cancellable,
                                NULL);
        
        avahi_service_browser_new_cb (output, user_data);

        g_main_loop_run(backend->loop);

        return;
}

static void 
_cph_cups_pappl_device_err_cb (const char        *message,
                               void              *data)
{        
        CphCups *cups = data;
        _cph_cups_set_internal_status ( cups, 
                          g_strdup_printf ("%s",message));
        
        return;
}

static bool
_cph_cups_pappl_device_cb (const char    *device_info,
                          const char    *device_uri,
                          const char    *device_id,
                          void          *user_data)
{
        CphCupsGetDevices *data = user_data;
        char              *key;

        if (data == NULL) 
        {
                return FALSE;
        }
        if (data->limit > 0 && data->iter >= data->limit)
                return FALSE;

        key = g_strdup_printf ("device_printer_application_missing:%d",data->iter);
                g_variant_builder_add (data->builder, "{ss}",
                                        key, "true");
        g_free (key);
        _cph_cups_get_devices_cb ( NULL, device_id, device_info, NULL, device_uri, NULL, NULL);
   
   return TRUE;
}

int _parse_app_devices (CphCups                 *cups,
                        CphCupsGetDevices       *data,
                        FILE                    *fp)
{
        char     buf[2048];
        char     device_uri[1024];
        char	 device_id[1024];

        while (fgets (buf, sizeof (buf), fp))
          {

	         strcpy (device_uri,buf);

                 device_uri[strlen (device_uri) - 1] = '\0';                
                _cph_cups_get_devices_cb ( NULL, device_id, NULL, NULL, device_uri, NULL, NULL);
          }

          return 0;     
}

static void 
get_printer_app_devices (gpointer  cb_data,
                        gpointer user_data)
{
        
        ipp_t		        *request,		
		                *response;		
        ipp_attribute_t         *attr;		
        http_t	                *http;			
        int                     timeout_param = CUPS_TIMEOUT_DEFAULT;  
        char                    *include_schemes_param,
                                *exclude_schemes_param,
                                *device_info,
                                *device_id,
                                *device_name,
                                *device_uri;
        AvahiData *printer_app = user_data;
        //   *data = printer_app->user_data; 
        http = httpConnect2(printer_app->hostname, printer_app->port, NULL, AF_UNSPEC,
                           HTTP_ENCRYPTION_IF_REQUESTED, 1, 30000, NULL);
        request = ippNewRequest(IPP_OP_PAPPL_FIND_DEVICES);
        ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_CHARSET, "attributes-charset", NULL, "utf-8");
        ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_LANGUAGE, "attributes-natural-language", NULL, "en-GB");
        ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "system-uri", NULL, "ipp://localhost/ipp/system");
        ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsUser());
        response = cupsDoRequest(http, request, "/ipp/system");         
        if ((attr = ippFindAttribute(response, "smi55357-device-col", IPP_TAG_BEGIN_COLLECTION)) != NULL)
          {
            int	i,			
        		num_devices = ippGetCount(attr);
            for (i = 0; i < num_devices; i ++)
            {
              ipp_t		*item = ippGetCollection(attr, i);
        					// Device entry
              ipp_attribute_t	*item_attr;	// Device attr
              if ((item_attr = ippFindAttribute(item, "smi55357-device-uri", IPP_TAG_ZERO)) != NULL)
              {
	         if ((item_attr = ippFindAttribute(item, "smi55357-device-info", IPP_TAG_ZERO)) != NULL)
      	           device_info = g_strdup(ippGetString(item_attr, 0, NULL));
      	         if ((item_attr = ippFindAttribute(item, "smi55357-device-id", IPP_TAG_ZERO)) != NULL)
      	           device_id = g_strdup(ippGetString(item_attr, 0, NULL));
                 if ((item_attr = ippFindAttribute(item, "smi55357-device-name", IPP_TAG_ZERO)) != NULL)
      	           device_name = g_strdup(ippGetString(item_attr, 0, NULL));
                 if ((item_attr = ippFindAttribute(item, "smi55357-device-uri", IPP_TAG_ZERO)) != NULL)
      	           device_uri = g_strdup(ippGetString(item_attr, 0, NULL));
                _cph_cups_get_devices_cb (NULL, 
                                         device_id,
                                         device_info,
                                         NULL,
                                         device_uri,
                                         NULL,
                                         user_data);
                 g_free (device_id);
                 g_free (device_info);
                 g_free (device_name);
                 g_free (device_uri);
              }
            }
          }

         ippDelete(response);                
}

static void 
discover_printer_app_devices_cb (gpointer  cb_data,
                                gpointer user_data)
{
        AvahiData *printer_app = cb_data;
        char                *key;
        CphCupsGetPrinterApps  *data = ((Avahi*)user_data)->data;

        if (printer_app->hostname && printer_app->hostname[0] != '\0') {
                        key  = g_strdup_printf ("hostname:%d", data->iter);
                        g_message("%s\n", key);
                        g_variant_builder_add (data->builder, "{ss}",
                                               key, printer_app->hostname);
                        g_free (key);
        }
        
        if (printer_app->port > 0) {
                key  = g_strdup_printf ("port:%d", data->iter);
                g_variant_builder_add (data->builder, "{ss}",
                                       key, g_strdup_printf("%i", printer_app->port));
                g_free (key);
        }
        data->iter++;
}

static gboolean
_cph_cups_printer_app_get (CphCups               *cups,
                           int                    timeout,
                           gpointer               data,
                           printer_app_cb          cb)
{

        Avahi               *printer_app_backend;

        printer_app_backend = g_new0 (Avahi,2);
        printer_app_backend[0].service_type = g_strdup_printf("_ipps-system._tcp");
        printer_app_backend[1].service_type = g_strdup_printf("_ipp-system._tcp");

        for(int i = 0 ; i < 2; i++)
                {
                        printer_app_backend[i].cups = cups;
                        printer_app_backend[i].data = data;
                        printer_app_backend[i].done = false;   
                        printer_app_backend[i].loop = g_main_loop_new(NULL, FALSE);                    
                        printer_app_backend[i].callback = cb; 
                        printer_app_backend[i].avahi_cancellable = g_cancellable_new ();
                        printer_app_backend[i].dbus_connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, printer_app_backend->avahi_cancellable, NULL);
                        avahi_create_browsers (&printer_app_backend[i]);
                }

        return TRUE;
}

static gboolean
_cph_cups_devices_get (CphCups           *cups,
                       int                timeout,
                       int                limit,
                       const char *const *include_schemes,
                       const char *const *exclude_schemes,
                       int                len_include,
                       int                len_exclude,
                       CphCupsGetDevices *data)
{
        ipp_status_t            retval;
        ipp_t		        *request,		
		                *response;		
        ipp_attribute_t         *attr;		
        http_t	                *http;			
        int                     timeout_param = CUPS_TIMEOUT_DEFAULT;  
        char                    *include_schemes_param,
                                *exclude_schemes_param,
                                *device_info,
                                *device_id,
                                *device_name,
                                *device_uri;
        if (timeout > 0)
                timeout_param = timeout;

        if (include_schemes && len_include > 0)
                include_schemes_param = g_strjoinv (",", (char **) include_schemes);
        else
                include_schemes_param = g_strdup (CUPS_INCLUDE_ALL);

        if (exclude_schemes && len_exclude > 0)
                exclude_schemes_param = g_strjoinv (",", (char **) exclude_schemes);
        else
                exclude_schemes_param = g_strdup (CUPS_EXCLUDE_NONE);

        // Discovering devices via lpinfo   

        retval = cupsGetDevices (cups->priv->connection,   
                                 timeout_param, 
                                 include_schemes_param,
                                 exclude_schemes_param,
                                 _cph_cups_get_devices_cb,
                                 data);

        // Discovering services via Avahi
        Avahi *printer_app_backend = g_new0 (Avahi,1);
        printer_app_backend -> cups = cups;

        // Polling devices from available Printer Apps
        _cph_cups_printer_app_get (cups, timeout, data, get_printer_app_devices);
        
        //  pappl_data_callback *pappl_data = g_new0 (pappl_data_callback,1);
        //  pappl_data->signal_pappl = 0;
        //  pappl_data->data = data;
        //  g_timeout_add_seconds (10,pappl_timeout_cb,pappl_data);
         
        //  papplDeviceList (PAPPL_DEVTYPE_ALL,
        //                   _cph_cups_pappl_device_cb,                  
        //                   pappl_data,
        //                   _cph_cups_pappl_device_err_cb,
        //                   cups );
           
	
        // if (retval != IPP_OK) {
        //         _cph_cups_set_internal_status (cups,
        //                                        "Cannot get devices.");
        //         return FALSE;
        // }

        g_free (include_schemes_param);
        g_free (exclude_schemes_param);
        
        return TRUE;
}

gboolean
cph_cups_devices_get (CphCups            *cups,
                      int                 timeout,
                      int                 limit,
                      const char *const  *include_schemes,
                      const char *const  *exclude_schemes,
                      GVariant          **devices)
{
        CphCupsGetDevices data;
        int               len_include;
        int               len_exclude;
        gboolean          retval;

        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);
        g_return_val_if_fail (devices != NULL, FALSE);

        *devices = NULL;

        /* check the validity of values */
        len_include = 0;
        if (include_schemes) {
                while (include_schemes[len_include] != NULL) {
                        if (!_cph_cups_is_scheme_valid (cups, include_schemes[len_include]))
                                return FALSE;
                        len_include++;
                }
        }

        len_exclude = 0;
        if (exclude_schemes) {
                while (exclude_schemes[len_exclude] != NULL) {
                        if (!_cph_cups_is_scheme_valid (cups, exclude_schemes[len_exclude]))
                                return FALSE;
                        len_exclude++;
                }
        }

        data.iter    = 0;
        data.limit   = -1;
        data.builder = g_variant_builder_new (G_VARIANT_TYPE ("a{ss}"));
        if (limit > 0)
                data.limit = limit;

        retval = _cph_cups_devices_get (cups, timeout, limit,
                                        include_schemes, exclude_schemes,
                                        len_include, len_exclude,
                                        &data);

        if (retval)
                *devices = g_variant_builder_end (data.builder);

        g_variant_builder_unref (data.builder);

        return retval;
}

gboolean
cph_cups_printer_app_get (CphCups            *cups,
                          int                 timeout,
                          GVariant          **apps)
{
        CphCupsGetPrinterApps   data;
        gboolean                retval;

        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);
        g_return_val_if_fail (apps != NULL, FALSE);

        *apps = NULL;

        data.iter    = 0;
        data.builder = g_variant_builder_new (G_VARIANT_TYPE ("a{ss}"));

        retval = _cph_cups_printer_app_get (cups,
                                            timeout, 
                                            &data,
                                            discover_printer_app_devices_cb);
        if (retval)
                *apps = g_variant_builder_end (data.builder);

        g_variant_builder_unref (data.builder);

        return retval;

}

/* Functions that work on a printer */

gboolean
cph_cups_printer_add (CphCups    *cups,
                      const char *printer_name,
                      const char *printer_uri,
                      const char *ppd_file,
                      const char *info,
                      const char *location)
{

        ipp_t		        *request,		
		                *response;		
        ipp_attribute_t         *attr;		
        http_t	                *http;			
        int                     timeout_param = CUPS_TIMEOUT_DEFAULT;

        http = httpConnect2("localhost", 8001, NULL, AF_UNSPEC,
                           HTTP_ENCRYPTION_IF_REQUESTED, 1, 30000, NULL);

        request = ippNewRequest(IPP_OP_PAPPL_FIND_DRIVERS);
        ippAddString(request, IPP_TAG_OPERATION, IPP_CONST_TAG(IPP_TAG_URI), "system-uri", NULL, "ipp://localhost/ipp/system");
        // I will pass here correct device id for devices
        //ippAddString(request, IPP_TAG_OPERATION, IPP_CONST_TAG(IPP_TAG_TEXT), "smi55357-device-id", NULL, "MFG:HP;CMD:PJL,PML,DW-PCL;MDL:HP LaserJet Tank MFP 260x;CLS:PRINTER;DES:HP LaserJet Tank MFP 2606dn;MEM:MEM=308MB;PRN:381U0A;S:0300000000000000000000000000000000;COMMENT:RES=600x2;LEDMDIS:USB#ff#04#01;CID:HPLJPCLMSMV2;MCT:MF;MCL:FL;MCV:4.2;");
        response = cupsDoRequest(http, request, "/ipp/system");

        attr = ippFindAttribute(response, "smi55357-driver-col", IPP_TAG_BEGIN_COLLECTION);
        const char *driver = NULL;

        if (attr != NULL) {
            driver = ippGetString(attr, 0, NULL);
        }

        request = ippNewRequest(IPP_OP_CREATE_PRINTER);

        ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "system-uri", NULL, "ipp://localhost/ipp/system");
        ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "printer-service-type", NULL, "print");
        ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "smi55357-driver", NULL, driver);
        ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "smi55357-device-uri", NULL, printer_uri);
        ippAddString(request, IPP_TAG_PRINTER, IPP_TAG_NAME, "printer-name", NULL, printer_name);
        // ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsUser());
        // return _cph_cups_send_request (cups, request, CPH_RESOURCE_ADMIN);

        response = cupsDoRequest(http, request, "/ipp/system");

        gboolean status = FALSE;
        if (cupsLastError() != IPP_STATUS_OK) {
                status = false;
        }
        ippDelete(response);
        httpClose(http);

        return status;

        // ipp_t *request;

        // g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        // if (!_cph_cups_is_printer_name_valid (cups, printer_name))
        //         return FALSE;
        // if (!_cph_cups_is_printer_uri_valid (cups, printer_uri))
        //         return FALSE;
        // if (!_cph_cups_is_ppd_valid (cups, ppd_file))
        //         return FALSE;
        // if (!_cph_cups_is_info_valid (cups, info))
        //         return FALSE;
        // if (!_cph_cups_is_location_valid (cups, location))
        //         return FALSE;

        // request = ippNewRequest (CUPS_ADD_MODIFY_PRINTER);
        // _cph_cups_add_printer_uri (request, printer_name);
        // _cph_cups_add_requesting_user_name (request, NULL);

        // ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_NAME,
        //               "printer-name", NULL, printer_name);

        // if (ppd_file && ppd_file[0] != '\0') {
        //         ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_NAME,
        //                       "ppd-name", NULL, ppd_file);
        // }
        // if (printer_uri && printer_uri[0] != '\0') {
        //         ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_URI,
        //                       "device-uri", NULL, printer_uri);
        // }
        // if (info && info[0] != '\0') {
        //         ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_TEXT,
        //                       "printer-info", NULL, info);
        // }
        // if (location && location[0] != '\0') {
        //         ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_TEXT,
        //                       "printer-location", NULL, location);
        // }

        // return _cph_cups_send_request (cups, request, CPH_RESOURCE_ADMIN);
}

gboolean
cph_cups_printer_app_printer_add(const char *printer_name,
                      const char *device_uri,
                      const char *device_info,
                      const char *device_id,
                      const char *hostname,
                      int *port)
{
        ipp_t		        *request,		
		                *response;		
        ipp_attribute_t         *attr;		
        http_t	                *http;			
        int                     timeout_param = CUPS_TIMEOUT_DEFAULT;

        http = httpConnect2("localhost", 8001, NULL, AF_UNSPEC,
                           HTTP_ENCRYPTION_IF_REQUESTED, 1, 30000, NULL);

        request = ippNewRequest(IPP_OP_CREATE_PRINTER);
        // ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "system-uri", NULL, "ipp://localhost/ipp/system");
        // ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "printer-service-type", NULL, "print");
        // ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "smi55357-driver", NULL, "HP 915");
        // ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "smi55357-device-uri", NULL, "ipps://Kaushikhp._ipps._tcp.local/");
        // ippAddString(request, IPP_TAG_PRINTER, IPP_TAG_NAME, "printer-name", NULL, "Kaushikhp2");

        ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "system-uri", NULL, "ipp://localhost/ipp/system");
        ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "printer-service-type", NULL, "print");
        ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "smi55357-driver", NULL, "auto");
        // ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "smi55357-device-uri", NULL, device_uri);
        // ippAddString(request, IPP_TAG_PRINTER, IPP_TAG_NAME, "printer-name", NULL, device_info);
        // ippAddString(request, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-device-id", NULL, device_id);
        ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "smi55357-device-uri", NULL, "usb://Canon/MF240%20Series%20UFRII%20LT?serial=0175B636DB5C&interface=1");
        ippAddString(request, IPP_TAG_PRINTER, IPP_TAG_NAME, "printer-name", NULL, "Canon MF240 Series UFRII LT");
        ippAddString(request, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-device-id", NULL, "MFG:Canon;MDL:MF240 Series UFRII LT;CMD:LIPSLX,CPCA;CLS:PRINTER;DES:Canon MF240 Series UFRII LT;CID:CA UFRII BW OIP;IPP-HTTP:T;IPP-E:07-01-04; PESP:V1;");
        ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsUser());
        response = cupsDoRequest(http, request, "/ipp/system");

        gboolean status = true;
        if (cupsLastError() != IPP_STATUS_OK) {
                status = false;
        }
        ippDelete(response);
        httpClose(http);

        return status;
}

gboolean
cph_cups_printer_add_with_ppd_file (CphCups    *cups,
                                    const char *printer_name,
                                    const char *printer_uri,
                                    const char *ppd_filename,
                                    const char *info,
                                    const char *location)
{
        ipp_t *request;

        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        if (!_cph_cups_is_printer_name_valid (cups, printer_name))
                return FALSE;
        if (!_cph_cups_is_printer_uri_valid (cups, printer_uri))
                return FALSE;
        if (!_cph_cups_is_ppd_filename_valid (cups, ppd_filename))
                return FALSE;
        if (!_cph_cups_is_info_valid (cups, info))
                return FALSE;
        if (!_cph_cups_is_location_valid (cups, location))
                return FALSE;

        request = ippNewRequest (CUPS_ADD_MODIFY_PRINTER);
        _cph_cups_add_printer_uri (request, printer_name);
        _cph_cups_add_requesting_user_name (request, NULL);

        ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_NAME,
                      "printer-name", NULL, printer_name);

        /* In this specific case of ADD_MODIFY, the URI can be NULL/empty since
         * we provide a complete PPD. And cups fails if we pass an empty
         * string. */
        if (printer_uri && printer_uri[0] != '\0') {
                ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_URI,
                              "device-uri", NULL, printer_uri);
        }

        if (info && info[0] != '\0') {
                ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_TEXT,
                              "printer-info", NULL, info);
        }
        if (location && location[0] != '\0') {
                ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_TEXT,
                              "printer-location", NULL, location);
        }

        return _cph_cups_post_request (cups, request, ppd_filename,
                                       CPH_RESOURCE_ADMIN);
}

gboolean
cph_cups_printer_delete (CphCups    *cups,
                         const char *printer_name)
{
        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        return _cph_cups_send_new_simple_request (cups, CUPS_DELETE_PRINTER,
                                                  printer_name,
                                                  CPH_RESOURCE_ADMIN);
}

gboolean
cph_cups_printer_set_default (CphCups    *cups,
                              const char *printer_name)
{
        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        return _cph_cups_send_new_simple_request (cups, CUPS_SET_DEFAULT,
                                                  printer_name,
                                                  CPH_RESOURCE_ADMIN);
}

gboolean
cph_cups_printer_set_enabled (CphCups    *cups,
                              const char *printer_name,
                              gboolean    enabled)
{
        ipp_op_t op;

        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        op = enabled ? IPP_RESUME_PRINTER : IPP_PAUSE_PRINTER;

        return _cph_cups_send_new_simple_request (cups, op, printer_name,
                                                  CPH_RESOURCE_ADMIN);
}

gboolean
cph_cups_printer_set_uri (CphCups    *cups,
                          const char *printer_name,
                          const char *printer_uri)
{
        ipp_t *request;

        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        if (!_cph_cups_is_printer_name_valid (cups, printer_name))
                return FALSE;
        if (!_cph_cups_is_printer_uri_valid (cups, printer_uri))
                return FALSE;

        request = ippNewRequest (CUPS_ADD_MODIFY_PRINTER);
        _cph_cups_add_printer_uri (request, printer_name);
        _cph_cups_add_requesting_user_name (request, NULL);

        ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_URI,
                      "device-uri", NULL, printer_uri);

        return _cph_cups_send_request (cups, request, CPH_RESOURCE_ADMIN);
}

/* reason must be NULL if accept is TRUE */
gboolean
cph_cups_printer_set_accept_jobs (CphCups    *cups,
                                  const char *printer_name,
                                  gboolean    accept,
                                  const char *reason)
{
        ipp_t *request;

        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);
        g_return_val_if_fail (!accept || reason == NULL, FALSE);

        if (!_cph_cups_is_printer_name_valid (cups, printer_name))
                return FALSE;
        if (!_cph_cups_is_reject_jobs_reason_valid (cups, reason))
                return FALSE;

        if (accept)
                return _cph_cups_send_new_simple_request (cups,
                                                          CUPS_ACCEPT_JOBS,
                                                          printer_name,
                                                          CPH_RESOURCE_ADMIN);

        /* !accept */
        request = ippNewRequest (CUPS_REJECT_JOBS);
        _cph_cups_add_printer_uri (request, printer_name);
        _cph_cups_add_requesting_user_name (request, NULL);

        if (reason && reason[0] == '\0')
                ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_TEXT,
                              "printer-state-message", NULL, reason);

        return _cph_cups_send_request (cups, request, CPH_RESOURCE_ADMIN);
}

/* Functions that work on a class */

gboolean
cph_cups_class_add_printer (CphCups    *cups,
                            const char *class_name,
                            const char *printer_name)
{
        int              printer_index;
        ipp_t           *reply;
        ipp_t           *request;
        int              new_len;
        ipp_attribute_t *printer_uris;
        char            *escaped_printer_name;
        char             printer_uri[HTTP_MAX_URI + 1];
        ipp_attribute_t *attr;

        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        if (!_cph_cups_is_class_name_valid (cups, class_name))
                return FALSE;
        if (!_cph_cups_is_printer_name_valid (cups, printer_name))
                return FALSE;

        /* check that the printer is not already in the class */
        printer_index = _cph_cups_class_has_printer (cups,
                                                     class_name, printer_name,
                                                     &reply);
        if (printer_index >= 0) {
                char *error;

                if (reply)
                        ippDelete (reply);

                error = g_strdup_printf ("Printer %s is already in class %s.",
                                         printer_name, class_name);
                _cph_cups_set_internal_status (cups, error);
                g_free (error);

                return FALSE;
        }

        /* add the printer to the class */

        request = ippNewRequest (CUPS_ADD_CLASS);
        _cph_cups_add_class_uri (request, class_name);
        _cph_cups_add_requesting_user_name (request, NULL);

        escaped_printer_name = g_uri_escape_string (printer_name, NULL, FALSE);
        g_snprintf (printer_uri, sizeof (printer_uri),
                    "ipp://localhost/printers/%s", escaped_printer_name);
        g_free (escaped_printer_name);

        /* new length: 1 + what we had before */
        new_len = 1;
        if (reply) {
                printer_uris = ippFindAttribute (reply,
                                                 "member-uris", IPP_TAG_URI);
                if (printer_uris)
                        new_len += ippGetCount (printer_uris);
        } else
                printer_uris = NULL;

        attr = ippAddStrings (request, IPP_TAG_PRINTER, IPP_TAG_URI,
                              "member-uris", new_len,
                              NULL, NULL);
        if (printer_uris) {
                int i;

                for (i = 0; i < ippGetCount (printer_uris); i++)
                        ippSetString (request, &attr, i,
                                      g_strdup (ippGetString (printer_uris, i, NULL)));
        }

        if (reply)
                ippDelete (reply);

        ippSetString (request, &attr, new_len - 1, g_strdup (printer_uri));

        return _cph_cups_send_request (cups, request, CPH_RESOURCE_ADMIN);
}

gboolean
cph_cups_class_delete_printer (CphCups    *cups,
                               const char *class_name,
                               const char *printer_name)
{
        int              printer_index;
        ipp_t           *reply;
        ipp_t           *request;
        int              new_len;
        ipp_attribute_t *printer_uris;
        ipp_attribute_t *attr;
        int              i;

        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        if (!_cph_cups_is_class_name_valid (cups, class_name))
                return FALSE;
        if (!_cph_cups_is_printer_name_valid (cups, printer_name))
                return FALSE;

        /* check that the printer is in the class */
        printer_index = _cph_cups_class_has_printer (cups,
                                                     class_name, printer_name,
                                                     &reply);
        /* Note: the second condition (!reply) is only here for safety purpose.
         * When it's TRUE, the first one should be TRUE too */
        if (printer_index < 0 || !reply) {
                char *error;

                if (reply)
                        ippDelete (reply);

                error = g_strdup_printf ("Printer %s is not in class %s.",
                                         printer_name, class_name);
                _cph_cups_set_internal_status (cups, error);
                g_free (error);

                return FALSE;
        }

        /* remove the printer from the class */

        /* new length: -1 + what we had before */
        new_len = -1;
        printer_uris = ippFindAttribute (reply,
                                         "member-uris", IPP_TAG_URI);
        if (printer_uris)
                new_len += ippGetCount (printer_uris);

        /* empty class: we delete it */
        if (new_len <= 0) {
                ippDelete (reply);
                return cph_cups_class_delete (cups, class_name);
        }

        /* printer_uris is not NULL and reply is not NULL */

        request = ippNewRequest (CUPS_ADD_CLASS);
        _cph_cups_add_class_uri (request, class_name);
        _cph_cups_add_requesting_user_name (request, NULL);

        attr = ippAddStrings (request, IPP_TAG_PRINTER, IPP_TAG_URI,
                              "member-uris", new_len,
                              NULL, NULL);

        /* copy all printers from the class, except the one we remove */
        for (i = 0; i < printer_index; i++)
                ippSetString (request, &attr, i,
                              g_strdup (ippGetString (printer_uris, i, NULL)));
        for (i = printer_index + 1; i < ippGetCount (printer_uris); i++)
                ippSetString (request, &attr, i,
                              g_strdup (ippGetString (printer_uris, i, NULL)));

        ippDelete (reply);

        return _cph_cups_send_request (cups, request, CPH_RESOURCE_ADMIN);
}

gboolean
cph_cups_class_delete (CphCups    *cups,
                       const char *class_name)
{
        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        return _cph_cups_send_new_simple_class_request (cups, CUPS_DELETE_CLASS,
                                                        class_name,
                                                        CPH_RESOURCE_ADMIN);
}

gboolean
cph_cups_printer_class_rename (CphCups    *cups,
                               const char *old_printer_name,
                               const char *new_printer_name)
{
        cups_dest_t      *dests;
        cups_dest_t      *dest;
        cups_job_t       *jobs;
        int               num_dests = 0;
        int               num_jobs = 0;
        ipp_t            *request;
        ipp_t            *response;
        ipp_t            *reply;
        ipp_attribute_t  *attr;
        gchar            *device_uri = NULL;
        gchar            *printer_info = NULL;
        gchar            *job_sheets = NULL;
        gchar            *printer_location = NULL;
        gchar            *printer_uri = NULL;
        gchar            *error_policy = NULL;
        gchar            *op_policy = NULL;
        gchar           **users_allowed = NULL;
        gchar           **users_denied = NULL;
        gchar           **member_names = NULL;
        const gchar      *ppd_link = NULL;
        gchar            *ppd_filename = NULL;
        gchar           **sheets = NULL;
        gchar            *start_sheet = NULL;
        gchar            *end_sheet = NULL;
        gboolean          accepting = FALSE;
        gboolean          printer_shared = FALSE;
        gboolean          printer_paused = FALSE;
        gboolean          is_default = FALSE;
        int               i;
        guint             len;

        static const char * const requested_attrs[] = {
                "printer-error-policy",
                "printer-op-policy",
                "requesting-user-name-allowed",
                "requesting-user-name-denied",
                "member-names"
        };

        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        if (!_cph_cups_is_printer_name_valid (cups, old_printer_name))
                return FALSE;
        if (!_cph_cups_is_printer_name_valid (cups, new_printer_name))
                return FALSE;

        num_dests = cupsGetDests (&dests);

        dest = cupsGetDest (new_printer_name, NULL, num_dests, dests);
        if (dest != NULL) {
                cupsFreeDests (num_dests, dests);
                return FALSE;
        }

        dest = cupsGetDest (old_printer_name, NULL, num_dests, dests);
        if (dest == NULL) {
                cupsFreeDests (num_dests, dests);
                return FALSE;
        }

        num_jobs = cupsGetJobs (&jobs, old_printer_name, 0, CUPS_WHICHJOBS_ACTIVE);
        for (i = 0; i < num_jobs; i++) {
                if (jobs[i].state == IPP_JSTATE_PENDING ||
                    jobs[i].state == IPP_JSTATE_PROCESSING) {
                        cupsFreeJobs (num_jobs, jobs);
                        cupsFreeDests (num_dests, dests);
                        return FALSE;
                }
        }
        cupsFreeJobs (num_jobs, jobs);

        for (i = 0; i < dest->num_options; i++) {
                if (g_strcmp0 (dest->options[i].name, "device-uri") == 0) {
                        device_uri = dest->options[i].value;
                } else if (g_strcmp0 (dest->options[i].name, "job-sheets") == 0) {
                        job_sheets = dest->options[i].value;
                } else if (g_strcmp0 (dest->options[i].name, "printer-info") == 0) {
                        printer_info = dest->options[i].value;
                } else if (g_strcmp0 (dest->options[i].name, "printer-is-accepting-jobs") == 0) {
                        accepting = g_strcmp0 (dest->options[i].value, "true") == 0;
                } else if (g_strcmp0 (dest->options[i].name, "printer-is-shared") == 0) {
                        printer_shared = g_strcmp0 (dest->options[i].value, "true") == 0;
                } else if (g_strcmp0 (dest->options[i].name, "printer-location") == 0) {
                        printer_location = dest->options[i].value;
                } else if (g_strcmp0 (dest->options[i].name, "printer-state") == 0) {
                        printer_paused = g_strcmp0 (dest->options[i].value, "5") == 0;
                } else if (g_strcmp0 (dest->options[i].name, "printer-uri-supported") == 0) {
                        printer_uri = dest->options[i].value;
                }
        }
        is_default = dest->is_default;

        request = ippNewRequest (IPP_GET_PRINTER_ATTRIBUTES);
        ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
                      "printer-uri", NULL, printer_uri);
        ippAddStrings (request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
                      "requested-attributes", G_N_ELEMENTS (requested_attrs), NULL, requested_attrs);
        response = cupsDoRequest (cups->priv->connection, request, "/");

        if (response != NULL) {
                if (ippGetStatusCode (response) <= IPP_OK_CONFLICT) {
                        attr = ippFindAttribute (response, "printer-error-policy", IPP_TAG_NAME);
                        if (attr != NULL)
                                error_policy = g_strdup (ippGetString (attr, 0, NULL));

                        attr = ippFindAttribute (response, "printer-op-policy", IPP_TAG_NAME);
                        if (attr != NULL)
                                op_policy = g_strdup (ippGetString (attr, 0, NULL));

                        attr = ippFindAttribute (response, "requesting-user-name-allowed", IPP_TAG_NAME);
                        if (attr != NULL && ippGetCount (attr) > 0) {
                                users_allowed = g_new0 (gchar *, ippGetCount (attr) + 1);
                                for (i = 0; i < ippGetCount (attr); i++)
                                        users_allowed[i] = g_strdup (ippGetString (attr, i, NULL));
                        }

                        attr = ippFindAttribute (response, "requesting-user-name-denied", IPP_TAG_NAME);
                        if (attr != NULL && ippGetCount (attr) > 0) {
                                users_denied = g_new0 (gchar *, ippGetCount (attr) + 1);
                                for (i = 0; i < ippGetCount (attr); i++)
                                        users_denied[i] = g_strdup (ippGetString (attr, i, NULL));
                        }

                        attr = ippFindAttribute (response, "member-names", IPP_TAG_NAME);
                        if (attr != NULL && ippGetCount (attr) > 0) {
                                member_names = g_new0 (gchar *, ippGetCount (attr) + 1);
                                for (i = 0; i < ippGetCount (attr); i++)
                                        member_names[i] = g_strdup (ippGetString (attr, i, NULL));
                        }
                }
                ippDelete (response);
        }

        ppd_link = cupsGetPPD (old_printer_name);
        if (ppd_link != NULL && (ppd_filename = g_file_read_link (ppd_link, NULL)) == NULL) {
                ppd_filename = g_strdup (ppd_link);
        }

        if (cph_cups_is_class (cups, old_printer_name)) {
                if (member_names != NULL) {
                        for (len = 0; len < g_strv_length (member_names); len++) {
                                cph_cups_class_add_printer (cups, new_printer_name, member_names[len]);
                        }
                }
        } else if (cph_cups_printer_add_with_ppd_file (cups,
                                                       new_printer_name,
                                                       device_uri,
                                                       ppd_filename,
                                                       printer_info,
                                                       printer_location)) {
                for (i = 0; i < num_dests; i++) {
                        if (cph_cups_is_class (cups, dests[i].name)) {
                                if (_cph_cups_class_has_printer (cups, dests[i].name, old_printer_name, &reply) >= 0) {
                                        if (reply != NULL)
                                                ippDelete (reply);
                                        cph_cups_class_delete_printer (cups, dests[i].name, old_printer_name);
                                        cph_cups_class_add_printer (cups, dests[i].name, new_printer_name);
                                }
                        }
                }
        } else {
                cph_cups_printer_set_accept_jobs (cups, old_printer_name, accepting, NULL);
                return FALSE;
        }

        num_jobs = cupsGetJobs (&jobs, old_printer_name, 0, CUPS_WHICHJOBS_ACTIVE);
        for (i = 0; i < num_jobs; i++) {
                if (jobs[i].state == IPP_JSTATE_HELD) {
                        request = ippNewRequest (CUPS_MOVE_JOB);

                        _cph_cups_add_job_uri (request, jobs[i].id);
                        _cph_cups_add_job_printer_uri (request, new_printer_name);
                        _cph_cups_add_requesting_user_name (request, cupsUser ());
                        _cph_cups_send_request (cups, request, CPH_RESOURCE_JOBS);
                }
        }
        cupsFreeJobs (num_jobs, jobs);

        cph_cups_printer_set_accept_jobs (cups, new_printer_name, accepting, NULL);
        if (is_default)
                cph_cups_printer_set_default (cups, new_printer_name);
        cph_cups_printer_class_set_error_policy (cups, new_printer_name, error_policy);
        cph_cups_printer_class_set_op_policy (cups, new_printer_name, op_policy);

        if (job_sheets != NULL) {
                sheets = g_strsplit (job_sheets, ",", 0);
                if (g_strv_length (sheets) > 1) {
                        start_sheet = sheets[0];
                        end_sheet = sheets[1];
                }
                cph_cups_printer_class_set_job_sheets (cups, new_printer_name, start_sheet, end_sheet);
        }
        cph_cups_printer_set_enabled (cups, new_printer_name, !printer_paused);
        cph_cups_printer_class_set_shared (cups, new_printer_name, printer_shared);
        cph_cups_printer_class_set_users_allowed (cups, new_printer_name, (const char * const *) users_allowed);
        cph_cups_printer_class_set_users_denied (cups, new_printer_name, (const char * const *) users_denied);

        if (cph_cups_is_class (cups, old_printer_name)) {
                if (member_names != NULL) {
                        for (len = 0; len < g_strv_length (member_names); len++) {
                                cph_cups_class_delete_printer (cups, old_printer_name, member_names[len]);
                        }
                }

                cph_cups_class_delete (cups, old_printer_name);
        } else {
                cph_cups_printer_delete (cups, old_printer_name);
        }


        cupsFreeDests (num_dests, dests);

        if (ppd_link != NULL) {
                g_unlink (ppd_link);
                g_free (ppd_filename);
        }
        g_free (op_policy);
        g_free (error_policy);
        g_strfreev (sheets);
        g_strfreev (users_allowed);
        g_strfreev (users_denied);

        return TRUE;
}

/* Functions that can work on printer and class */

gboolean
cph_cups_printer_class_set_info (CphCups    *cups,
                                 const char *printer_name,
                                 const char *info)
{
        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        if (!_cph_cups_is_printer_name_valid (cups, printer_name))
                return FALSE;
        if (!_cph_cups_is_info_valid (cups, info))
                return FALSE;

        return _cph_cups_send_new_printer_class_request (cups, printer_name,
                                                         IPP_TAG_PRINTER,
                                                         IPP_TAG_TEXT,
                                                         "printer-info",
                                                         info);
}

gboolean
cph_cups_printer_class_set_location (CphCups    *cups,
                                     const char *printer_name,
                                     const char *location)
{
        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        if (!_cph_cups_is_printer_name_valid (cups, printer_name))
                return FALSE;
        if (!_cph_cups_is_location_valid (cups, location))
                return FALSE;

        return _cph_cups_send_new_printer_class_request (cups, printer_name,
                                                         IPP_TAG_PRINTER,
                                                         IPP_TAG_TEXT,
                                                         "printer-location",
                                                         location);
}

gboolean
cph_cups_printer_class_set_shared (CphCups    *cups,
                                   const char *printer_name,
                                   gboolean    shared)
{
        ipp_t *request;

        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        if (!_cph_cups_is_printer_name_valid (cups, printer_name))
                return FALSE;

        request = ippNewRequest (CUPS_ADD_MODIFY_PRINTER);
        _cph_cups_add_printer_uri (request, printer_name);
        _cph_cups_add_requesting_user_name (request, NULL);
        ippAddBoolean (request, IPP_TAG_OPERATION,
                       "printer-is-shared", shared ? 1 : 0);

        if (_cph_cups_send_request (cups, request, CPH_RESOURCE_ADMIN))
                return TRUE;

        /* it failed, maybe it was a class? */
        if (cups->priv->last_status != IPP_NOT_POSSIBLE)
                return FALSE;

        request = ippNewRequest (CUPS_ADD_MODIFY_CLASS);
        _cph_cups_add_class_uri (request, printer_name);
        _cph_cups_add_requesting_user_name (request, NULL);
        ippAddBoolean (request, IPP_TAG_OPERATION,
                       "printer-is-shared", shared ? 1 : 0);

        return _cph_cups_send_request (cups, request, CPH_RESOURCE_ADMIN);
}

gboolean
cph_cups_printer_class_set_job_sheets (CphCups    *cups,
                                       const char *printer_name,
                                       const char *start,
                                       const char *end)
{
        ipp_t *request;
        const char * const values[2] = { start, end };

        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        if (!_cph_cups_is_printer_name_valid (cups, printer_name))
                return FALSE;
        if (!_cph_cups_is_job_sheet_valid (cups, start))
                return FALSE;
        if (!_cph_cups_is_job_sheet_valid (cups, end))
                return FALSE;

        request = ippNewRequest (CUPS_ADD_MODIFY_PRINTER);
        _cph_cups_add_printer_uri (request, printer_name);
        _cph_cups_add_requesting_user_name (request, NULL);
        ippAddStrings (request, IPP_TAG_PRINTER, IPP_TAG_NAME,
                       "job-sheets-default", 2, NULL, values);

        if (_cph_cups_send_request (cups, request, CPH_RESOURCE_ADMIN))
                return TRUE;

        /* it failed, maybe it was a class? */
        if (cups->priv->last_status != IPP_NOT_POSSIBLE)
                return FALSE;

        request = ippNewRequest (CUPS_ADD_MODIFY_CLASS);
        _cph_cups_add_class_uri (request, printer_name);
        _cph_cups_add_requesting_user_name (request, NULL);
        ippAddStrings (request, IPP_TAG_PRINTER, IPP_TAG_NAME,
                       "job-sheets-default", 2, NULL, values);

        return _cph_cups_send_request (cups, request, CPH_RESOURCE_ADMIN);
}

gboolean
cph_cups_printer_class_set_error_policy (CphCups    *cups,
                                         const char *printer_name,
                                         const char *policy)
{
        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        if (!_cph_cups_is_printer_name_valid (cups, printer_name))
                return FALSE;
        if (!_cph_cups_is_error_policy_valid (cups, policy))
                return FALSE;

        return _cph_cups_send_new_printer_class_request (cups, printer_name,
                                                         IPP_TAG_PRINTER,
                                                         IPP_TAG_NAME,
                                                         "printer-error-policy",
                                                         policy);
}

gboolean
cph_cups_printer_class_set_op_policy (CphCups    *cups,
                                      const char *printer_name,
                                      const char *policy)
{
        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        if (!_cph_cups_is_printer_name_valid (cups, printer_name))
                return FALSE;
        if (!_cph_cups_is_op_policy_valid (cups, policy))
                return FALSE;

        return _cph_cups_send_new_printer_class_request (cups, printer_name,
                                                         IPP_TAG_PRINTER,
                                                         IPP_TAG_NAME,
                                                         "printer-op-policy",
                                                         policy);
}

/* set users to NULL to allow all users */
gboolean
cph_cups_printer_class_set_users_allowed (CphCups           *cups,
                                          const char        *printer_name,
                                          const char *const *users)
{
        int len;

        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        if (!_cph_cups_is_printer_name_valid (cups, printer_name))
                return FALSE;
        /* check the validity of values, and get the length of the array at the
         * same time */
        len = 0;
        if (users) {
                while (users[len] != NULL) {
                        if (!_cph_cups_is_user_valid (cups, users[len]))
                                return FALSE;
                        len++;
                }
        }

        return _cph_cups_printer_class_set_users (cups, printer_name, users,
                                                  "requesting-user-name-allowed",
                                                  "all");
}

/* set users to NULL to deny no user */
gboolean
cph_cups_printer_class_set_users_denied (CphCups           *cups,
                                         const char        *printer_name,
                                         const char *const *users)
{
        int len;

        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        if (!_cph_cups_is_printer_name_valid (cups, printer_name))
                return FALSE;
        /* check the validity of values, and get the length of the array at the
         * same time */
        len = 0;
        if (users) {
                while (users[len] != NULL) {
                        if (!_cph_cups_is_user_valid (cups, users[len]))
                                return FALSE;
                        len++;
                }
        }

        return _cph_cups_printer_class_set_users (cups, printer_name, users,
                                                  "requesting-user-name-denied",
                                                  "none");
}

/* set values to NULL to delete the default */
gboolean
cph_cups_printer_class_set_option_default (CphCups           *cups,
                                           const char        *printer_name,
                                           const char        *option,
                                           const char *const *values)
{
        gboolean         is_class;
        char            *option_name;
        int              len;
        ipp_t           *request;
        ipp_attribute_t *attr;
        gboolean         retval;

        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        if (!_cph_cups_is_printer_name_valid (cups, printer_name))
                return FALSE;
        if (!_cph_cups_is_option_valid (cups, option))
                return FALSE;
        /* check the validity of values, and get the length of the array at the
         * same time */
        len = 0;
        if (values) {
                while (values[len] != NULL) {
                        if (!_cph_cups_is_option_value_valid (cups,
                                                              values[len]))
                                return FALSE;
                        len++;
                }
        }

        option_name = g_strdup_printf ("%s-default", option);

        /* delete default value for option */
        if (len == 0) {
                retval = _cph_cups_send_new_printer_class_request (
                                                        cups,
                                                        printer_name,
                                                        IPP_TAG_PRINTER,
                                                        IPP_TAG_DELETEATTR,
                                                        option_name,
                                                        NULL);
                g_free (option_name);

                return retval;
        }

        /* set default value for option */
        is_class = cph_cups_is_class (cups, printer_name);

        if (is_class) {
                request = ippNewRequest (CUPS_ADD_MODIFY_CLASS);
                _cph_cups_add_class_uri (request, printer_name);
        } else {
                request = ippNewRequest (CUPS_ADD_MODIFY_PRINTER);
                _cph_cups_add_printer_uri (request, printer_name);
        }

        _cph_cups_add_requesting_user_name (request, NULL);

        if (len == 1)
                ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_NAME,
                              option_name, NULL, values[0]);
        else {
                int i;

                attr = ippAddStrings (request, IPP_TAG_PRINTER, IPP_TAG_NAME,
                                      option_name, len, NULL, NULL);

                for (i = 0; i < len; i++)
                        ippSetString (request, &attr, i, g_strdup (values[i]));
        }

        retval = _cph_cups_send_request (cups, request, CPH_RESOURCE_ADMIN);

        g_free (option_name);

        return retval;
}

/* This function sets given options to specified values in file 'ppdfile'.
 * This needs to be done because of applications which use content of PPD files
 * instead of IPP attributes.
 * CUPS doesn't do this automatically (but hopefully will starting with 1.6) */
static gchar *
_cph_cups_prepare_ppd_for_options (CphCups       *cups,
                                   const gchar   *ppdfile,
                                   cups_option_t *options,
                                   gint           num_options)
{
        ppd_file_t   *ppd;
        gboolean      ppdchanged = FALSE;
        gchar        *result = NULL;
        gchar        *error;
        char          newppdfile[CPH_PATH_MAX];
        cups_file_t  *in = NULL;
        cups_file_t  *out = NULL;
        char          line[CPH_STR_MAXLEN];
        char          keyword[CPH_STR_MAXLEN];
        char         *keyptr;
        ppd_choice_t *choice;
        const char   *value;

        ppd = ppdOpenFile (ppdfile);
        if (!ppd) {
                error = g_strdup_printf ("Unable to open PPD file \"%s\": %s",
                                         ppdfile, strerror (errno));
                _cph_cups_set_internal_status (cups, error);
                g_free (error);

                goto out;
        }

        in = cupsFileOpen (ppdfile, "r");
        if (!in) {
                error = g_strdup_printf ("Unable to open PPD file \"%s\": %s",
                                         ppdfile, strerror (errno));
                _cph_cups_set_internal_status (cups, error);
                g_free (error);

                goto out;
        }

        out = cupsTempFile2 (newppdfile, sizeof (newppdfile));
        if (!out) {
                _cph_cups_set_internal_status (cups,
                                               "Unable to create temporary file");

                goto out;
        }

        /* Mark default values and values of options we are changing. */
        ppdMarkDefaults (ppd);
        cupsMarkOptions (ppd, num_options, options);

        while (cupsFileGets (in, line, sizeof (line))) {
                if (!g_str_has_prefix (line, "*Default")) {
                        cupsFilePrintf (out, "%s\n", line);
                } else {
                        /* This part parses lines with *Default on their
                         * beginning. For instance:
                         *   "*DefaultResolution: 1200dpi" becomes:
                         *     - keyword: Resolution
                         *     - keyptr: 1200dpi
                         */
                        g_strlcpy (keyword,
                                   line + strlen ("*Default"),
                                   sizeof (keyword));

                        for (keyptr = keyword; *keyptr; keyptr++)
                                if (*keyptr == ':' || isspace (*keyptr & 255))
                                        break;

                        *keyptr++ = '\0';
                        while (isspace (*keyptr & 255))
                                keyptr++;

                        /* We have to change PageSize if any of PageRegion,
                         * PageSize, PaperDimension or ImageableArea changes.
                         * We change PageRegion if PageSize is not available. */
                        if (g_str_equal (keyword, "PageRegion") ||
                            g_str_equal (keyword, "PageSize") ||
                            g_str_equal (keyword, "PaperDimension") ||
                            g_str_equal (keyword, "ImageableArea")) {
                                choice = ppdFindMarkedChoice (ppd, "PageSize");
                                if (!choice)
                                        choice = ppdFindMarkedChoice (ppd, "PageRegion");
                        } else {
                                choice = ppdFindMarkedChoice (ppd, keyword);
                        }

                        if (choice && !g_str_equal (choice->choice, keyptr)) {
                                /* We have to set the value in PPD manually if
                                 * a custom value was passed in:
                                 * cupsMarkOptions() marks the choice as
                                 * "Custom". We want to set this value with our
                                 * input. */
                                if (!g_str_equal (choice->choice, "Custom")) {
                                        cupsFilePrintf (out,
                                                        "*Default%s: %s\n",
                                                        keyword,
                                                        choice->choice);
                                        ppdchanged = TRUE;
                                } else {
                                        value = cupsGetOption (keyword,
                                                               num_options,
                                                               options);
                                        if (value) {
                                                cupsFilePrintf (out,
                                                                "*Default%s: %s\n",
                                                                keyword,
                                                                value);
                                                ppdchanged = TRUE;
                                        } else {
                                                cupsFilePrintf (out,
                                                                "%s\n", line);
                                        }
                                }
                        } else {
                                cupsFilePrintf (out, "%s\n", line);
                        }
                }
        }

        if (ppdchanged)
                result = g_strdup (newppdfile);
        else
                g_unlink (newppdfile);

out:
        if (in)
                cupsFileClose (in);
        if (out)
                cupsFileClose (out);
        if (ppd)
                ppdClose (ppd);

        return result;
}

gboolean
cph_cups_printer_class_set_option (CphCups           *cups,
                                   const char        *printer_name,
                                   const char        *option,
                                   const char *const *values)
{
        gboolean         is_class;
        int              len;
        ipp_t           *request;
        ipp_attribute_t *attr;
        char            *newppdfile;
        gboolean         retval;

        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        if (!_cph_cups_is_printer_name_valid (cups, printer_name))
                return FALSE;
        if (!_cph_cups_is_option_valid (cups, option))
                return FALSE;
        /* check the validity of values, and get the length of the array at the
         * same time */
        len = 0;
        if (values) {
                while (values[len] != NULL) {
                        if (!_cph_cups_is_option_value_valid (cups,
                                                              values[len]))
                                return FALSE;
                        len++;
                }
        }

        if (len == 0)
                return FALSE;

        is_class = cph_cups_is_class (cups, printer_name);

        /* We permit only one value to change in PPD file because we are setting
         * default value in it. */
        if (!is_class && len == 1) {
                cups_option_t *options = NULL;
                int            num_options = 0;
                char          *ppdfile = NULL;

                num_options = cupsAddOption (option, values[0], num_options, &options);

                ppdfile = g_strdup (cupsGetPPD (printer_name));

                newppdfile = _cph_cups_prepare_ppd_for_options (cups, ppdfile, options, num_options);

                g_unlink (ppdfile);
                g_free (ppdfile);
                cupsFreeOptions (num_options, options);
        } else
                newppdfile = NULL;

        if (is_class) {
                request = ippNewRequest (CUPS_ADD_MODIFY_CLASS);
                _cph_cups_add_class_uri (request, printer_name);
        } else {
                request = ippNewRequest (CUPS_ADD_MODIFY_PRINTER);
                _cph_cups_add_printer_uri (request, printer_name);
        }

        _cph_cups_add_requesting_user_name (request, NULL);

        if (len == 1) {
                ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_NAME,
                              option, NULL, values[0]);
        } else {
                int i;

                attr = ippAddStrings (request, IPP_TAG_PRINTER, IPP_TAG_NAME,
                                      option, len, NULL, NULL);

                for (i = 0; i < len; i++)
                        ippSetString (request, &attr, i, g_strdup (values[i]));
        }

        if (newppdfile) {
                retval = _cph_cups_post_request (cups, request, newppdfile, CPH_RESOURCE_ADMIN);
                g_unlink (newppdfile);
                g_free (newppdfile);
        } else {
                retval = _cph_cups_send_request (cups, request, CPH_RESOURCE_ADMIN);
        }

        return retval;
}

/* Functions that work on jobs */

gboolean
cph_cups_job_cancel (CphCups    *cups,
                     int         job_id,
                     gboolean    purge_job,
                     const char *user_name)
{
        ipp_t *request;

        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        if (!_cph_cups_is_job_id_valid (cups, job_id))
                return FALSE;
        /* we don't check if the user name is valid or not because it comes
         * from getpwuid(), and not dbus */

        request = ippNewRequest (IPP_CANCEL_JOB);
        _cph_cups_add_job_uri (request, job_id);

        if (user_name != NULL)
                _cph_cups_add_requesting_user_name (request, user_name);

        if (purge_job)
                ippAddBoolean (request, IPP_TAG_OPERATION, "purge-job", 1);

        return _cph_cups_send_request (cups, request, CPH_RESOURCE_JOBS);
}

gboolean
cph_cups_job_restart (CphCups    *cups,
                      int         job_id,
                      const char *user_name)
{
        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        if (!_cph_cups_is_job_id_valid (cups, job_id))
                return FALSE;
        /* we don't check if the user name is valid or not because it comes
         * from getpwuid(), and not dbus */

        return _cph_cups_send_new_simple_job_request (cups, IPP_RESTART_JOB,
                                                      job_id,
                                                      user_name,
                                                      CPH_RESOURCE_JOBS);
}

gboolean
cph_cups_job_set_hold_until (CphCups    *cups,
                             int         job_id,
                             const char *job_hold_until,
                             const char *user_name)
{
        g_return_val_if_fail (CPH_IS_CUPS (cups), FALSE);

        if (!_cph_cups_is_job_id_valid (cups, job_id))
                return FALSE;
        if (!_cph_cups_is_job_hold_until_valid (cups, job_hold_until))
                return FALSE;
        /* we don't check if the user name is valid or not because it comes
         * from getpwuid(), and not dbus */

        return _cph_cups_send_new_job_attributes_request (cups,
                                                          job_id,
                                                          "job-hold-until",
                                                          job_hold_until,
                                                          user_name,
                                                          CPH_RESOURCE_JOBS);
}

CphJobStatus
cph_cups_job_get_status (CphCups    *cups,
                         int         job_id,
                         const char *user)
{
        const char * const  attrs[1] = { "job-originating-user-name" };
        ipp_t              *request;
        const char         *resource_char;
        ipp_t              *reply;
        const char         *orig_user;
        CphJobStatus        status;

        g_return_val_if_fail (CPH_IS_CUPS (cups), CPH_JOB_STATUS_INVALID);

        if (!_cph_cups_is_job_id_valid (cups, job_id))
                return CPH_JOB_STATUS_INVALID;

        request = ippNewRequest (IPP_GET_JOB_ATTRIBUTES);
        _cph_cups_add_job_uri (request, job_id);
        ippAddStrings (request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
                       "requested-attributes", 1, NULL, attrs);
        /* Request attributes explicitly as the user running the process (as
         * opposed to the user doing the dbus call). This is root in general,
         * so we'll be authorized to get attributes for all jobs. */
        _cph_cups_add_requesting_user_name (request, NULL);

        resource_char = _cph_cups_get_resource (CPH_RESOURCE_ROOT);
        reply = cupsDoRequest (cups->priv->connection,
                               request, resource_char);

        if (!_cph_cups_is_reply_ok (cups, reply, TRUE))
                return CPH_JOB_STATUS_INVALID;

        orig_user = _cph_cups_get_attribute_string (reply, IPP_TAG_JOB,
                                                    attrs[0], IPP_TAG_NAME);

        status = CPH_JOB_STATUS_INVALID;

        if (orig_user) {
                if (g_strcmp0 (orig_user, user) == 0)
                        status = CPH_JOB_STATUS_OWNED_BY_USER;
                else
                        status = CPH_JOB_STATUS_NOT_OWNED_BY_USER;
        }

        ippDelete (reply);

        return status;
}

/******************************************************
 * Non-object functions
 ******************************************************/

gboolean
cph_cups_is_printer_uri_local (const char *uri)
{
        char *lower_uri;

        g_return_val_if_fail (uri != NULL, FALSE);

        /* empty URI: can only be local... */
        if (uri[0] == '\0')
                return TRUE;

        lower_uri = g_ascii_strdown (uri, -1);

        /* clearly local stuff */
        if (g_str_has_prefix (lower_uri, "parallel:") ||
            g_str_has_prefix (lower_uri, "usb:") ||
            g_str_has_prefix (lower_uri, "hal:") ||
            /* beh is the backend error handler */
            g_str_has_prefix (lower_uri, "beh:") ||
            g_str_has_prefix (lower_uri, "scsi:") ||
            g_str_has_prefix (lower_uri, "serial:") ||
            g_str_has_prefix (lower_uri, "file:") ||
            g_str_has_prefix (lower_uri, "pipe:")) {
                g_free (lower_uri);
                return TRUE;
        }

        /* clearly remote stuff */
        if (g_str_has_prefix (lower_uri, "socket:") ||
            g_str_has_prefix (lower_uri, "ipp:") ||
            g_str_has_prefix (lower_uri, "http:") ||
            g_str_has_prefix (lower_uri, "lpd:") ||
            g_str_has_prefix (lower_uri, "smb:") ||
            g_str_has_prefix (lower_uri, "novell:")) {
                g_free (lower_uri);
                return FALSE;
        }

        /* hplip can be both, I think. Let's just check if we have an ip
         * argument in the URI */
        if (g_str_has_prefix (lower_uri, "hp:") ||
            g_str_has_prefix (lower_uri, "hpfax:")) {
                char *buf;

                buf = strchr (lower_uri, '?');

                while (buf) {
                        buf++;
                        if (g_str_has_prefix (buf, "ip="))
                                break;
                        buf = strchr (buf, '&');
                }

                g_free (lower_uri);
                return buf == NULL;
        }

        g_free (lower_uri);

        /* we don't know, so we assume it's not local */
        return FALSE;
}
