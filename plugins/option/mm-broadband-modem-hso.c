/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your hso) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2008 - 2009 Novell, Inc.
 * Copyright (C) 2009 - 2012 Red Hat, Inc.
 * Copyright (C) 2012 Aleksander Morgado <aleksander@gnu.org>
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "ModemManager.h"
#include "mm-modem-helpers.h"
#include "mm-log.h"
#include "mm-errors-types.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-iface-modem-location.h"
#include "mm-base-modem-at.h"
#include "mm-broadband-modem-hso.h"
#include "mm-broadband-bearer-hso.h"
#include "mm-bearer-list.h"

static void iface_modem_init (MMIfaceModem *iface);
static void iface_modem_3gpp_init (MMIfaceModem3gpp *iface);
static void iface_modem_location_init (MMIfaceModemLocation *iface);

static MMIfaceModem3gpp *iface_modem_3gpp_parent;
static MMIfaceModemLocation *iface_modem_location_parent;

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemHso, mm_broadband_modem_hso, MM_TYPE_BROADBAND_MODEM_OPTION, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_3GPP, iface_modem_3gpp_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_LOCATION, iface_modem_location_init));

struct _MMBroadbandModemHsoPrivate {
    /* Regex for connected notifications */
    GRegex *_owancall_regex;
};

/*****************************************************************************/
/* Create Bearer (Modem interface) */

static MMBearer *
modem_create_bearer_finish (MMIfaceModem *self,
                            GAsyncResult *res,
                            GError **error)
{
    MMBearer *bearer;

    bearer = g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res));
    mm_dbg ("New HSO bearer created at DBus path '%s'", mm_bearer_get_path (bearer));

    return g_object_ref (bearer);
}

static void
broadband_bearer_hso_new_ready (GObject *source,
                                GAsyncResult *res,
                                GSimpleAsyncResult *simple)
{
    MMBearer *bearer = NULL;
    GError *error = NULL;

    bearer = mm_broadband_bearer_hso_new_finish (res, &error);
    if (!bearer)
        g_simple_async_result_take_error (simple, error);
    else
        g_simple_async_result_set_op_res_gpointer (simple,
                                                   bearer,
                                                   (GDestroyNotify)g_object_unref);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
modem_create_bearer (MMIfaceModem *self,
                     MMBearerProperties *properties,
                     GAsyncReadyCallback callback,
                     gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_create_bearer);

    mm_dbg ("Creating HSO bearer...");
    mm_broadband_bearer_hso_new (MM_BROADBAND_MODEM_HSO (self),
                                 properties,
                                 NULL, /* cancellable */
                                 (GAsyncReadyCallback)broadband_bearer_hso_new_ready,
                                 result);
}

/*****************************************************************************/
/* Setup/Cleanup unsolicited events (3GPP interface) */

typedef struct {
    guint cid;
    MMBroadbandBearerHsoConnectionStatus status;
} BearerListReportStatusForeachContext;

static void
bearer_list_report_status_foreach (MMBearer *bearer,
                                   BearerListReportStatusForeachContext *ctx)
{
    if (mm_broadband_bearer_get_3gpp_cid (MM_BROADBAND_BEARER (bearer)) != ctx->cid)
        return;

    mm_broadband_bearer_hso_report_connection_status (MM_BROADBAND_BEARER_HSO (bearer),
                                                      ctx->status);
}

static void
hso_connection_status_changed (MMAtSerialPort *port,
                               GMatchInfo *match_info,
                               MMBroadbandModemHso *self)
{
    MMBearerList *list = NULL;
    BearerListReportStatusForeachContext ctx;
    guint cid;
    guint status;

    /* Ensure we got proper parsed values */
    if (!mm_get_uint_from_match_info (match_info, 1, &cid) ||
        !mm_get_uint_from_match_info (match_info, 2, &status))
        return;

    /* Setup context */
    ctx.cid = 0;
    ctx.status = MM_BROADBAND_BEARER_HSO_CONNECTION_STATUS_UNKNOWN;

    switch (status) {
    case 1:
        ctx.status = MM_BROADBAND_BEARER_HSO_CONNECTION_STATUS_CONNECTED;
        break;
    case 3:
        ctx.status = MM_BROADBAND_BEARER_HSO_CONNECTION_STATUS_CONNECTION_FAILED;
        break;
    case 0:
        ctx.status = MM_BROADBAND_BEARER_HSO_CONNECTION_STATUS_DISCONNECTED;
        break;
    default:
        break;
    }

    /* If unknown status, don't try to report anything */
    if (ctx.status == MM_BROADBAND_BEARER_HSO_CONNECTION_STATUS_UNKNOWN)
        return;

    /* If empty bearer list, nothing else to do */
    g_object_get (self,
                  MM_IFACE_MODEM_BEARER_LIST, &list,
                  NULL);
    if (!list)
        return;

    /* Will report status only in the bearer with the specific CID */
    mm_bearer_list_foreach (list,
                            (MMBearerListForeachFunc)bearer_list_report_status_foreach,
                            &ctx);
    g_object_unref (list);
}

static gboolean
modem_3gpp_setup_cleanup_unsolicited_events_finish (MMIfaceModem3gpp *self,
                                                    GAsyncResult *res,
                                                    GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void
parent_setup_unsolicited_events_ready (MMIfaceModem3gpp *self,
                                       GAsyncResult *res,
                                       GSimpleAsyncResult *simple)
{
    GError *error = NULL;

    if (!iface_modem_3gpp_parent->setup_unsolicited_events_finish (self, res, &error))
        g_simple_async_result_take_error (simple, error);
    else {
        /* Our own setup now */
        mm_at_serial_port_add_unsolicited_msg_handler (
            mm_base_modem_peek_port_primary (MM_BASE_MODEM (self)),
            MM_BROADBAND_MODEM_HSO (self)->priv->_owancall_regex,
            (MMAtSerialUnsolicitedMsgFn)hso_connection_status_changed,
            self,
            NULL);

        g_simple_async_result_set_op_res_gboolean (G_SIMPLE_ASYNC_RESULT (res), TRUE);
    }

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
modem_3gpp_setup_unsolicited_events (MMIfaceModem3gpp *self,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_3gpp_setup_unsolicited_events);

    /* Chain up parent's setup */
    iface_modem_3gpp_parent->setup_unsolicited_events (
        self,
        (GAsyncReadyCallback)parent_setup_unsolicited_events_ready,
        result);
}

static void
parent_cleanup_unsolicited_events_ready (MMIfaceModem3gpp *self,
                                         GAsyncResult *res,
                                         GSimpleAsyncResult *simple)
{
    GError *error = NULL;

    if (!iface_modem_3gpp_parent->cleanup_unsolicited_events_finish (self, res, &error))
        g_simple_async_result_take_error (simple, error);
    else
        g_simple_async_result_set_op_res_gboolean (G_SIMPLE_ASYNC_RESULT (res), TRUE);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
modem_3gpp_cleanup_unsolicited_events (MMIfaceModem3gpp *self,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_3gpp_cleanup_unsolicited_events);

    /* Our own cleanup first */
    mm_at_serial_port_add_unsolicited_msg_handler (
        mm_base_modem_peek_port_primary (MM_BASE_MODEM (self)),
        MM_BROADBAND_MODEM_HSO (self)->priv->_owancall_regex,
        NULL, NULL, NULL);

    /* And now chain up parent's cleanup */
    iface_modem_3gpp_parent->cleanup_unsolicited_events (
        self,
        (GAsyncReadyCallback)parent_cleanup_unsolicited_events_ready,
        result);
}

/*****************************************************************************/
/* Location capabilities loading (Location interface) */

static MMModemLocationSource
location_load_capabilities_finish (MMIfaceModemLocation *self,
                                   GAsyncResult *res,
                                   GError **error)
{
    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return MM_MODEM_LOCATION_SOURCE_NONE;

    return (MMModemLocationSource) GPOINTER_TO_UINT (g_simple_async_result_get_op_res_gpointer (
                                                         G_SIMPLE_ASYNC_RESULT (res)));
}

static void
parent_load_capabilities_ready (MMIfaceModemLocation *self,
                                GAsyncResult *res,
                                GSimpleAsyncResult *simple)
{
    MMModemLocationSource sources;
    GError *error = NULL;

    sources = iface_modem_location_parent->load_capabilities_finish (self, res, &error);
    if (error) {
        g_simple_async_result_take_error (simple, error);
        g_simple_async_result_complete (simple);
        g_object_unref (simple);
        return;
    }

    /* Now our own check.
     *
     * We could issue AT_OIFACE? to list the interfaces currently enabled in the
     * module, to see if there is a 'GPS' interface enabled. But we'll just go
     * and see if there is already a 'GPS control' AT port and a raw serial 'GPS'
     * port grabbed.
     *
     * NOTE: A deeper implementation could handle the situation where the GPS
     * interface is found disabled in AT_OIFACE?. In this case, we could issue
     * AT_OIFACE="GPS",1 to enable it (and AT_OIFACE="GPS",0 to disable it), but
     * enabling/disabling GPS involves a complete reboot of the modem, which is
     * probably not the desired thing here.
     */
    if (mm_base_modem_peek_port_gps (MM_BASE_MODEM (self)) &&
        mm_base_modem_peek_port_gps_control (MM_BASE_MODEM (self)))
        sources |= MM_MODEM_LOCATION_SOURCE_GPS_NMEA;

    /* So we're done, complete */
    g_simple_async_result_set_op_res_gpointer (simple,
                                               GUINT_TO_POINTER (sources),
                                               NULL);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
location_load_capabilities (MMIfaceModemLocation *self,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        location_load_capabilities);

    /* Chain up parent's setup */
    iface_modem_location_parent->load_capabilities (
        self,
        (GAsyncReadyCallback)parent_load_capabilities_ready,
        result);
}

/*****************************************************************************/
/* Enable/Disable location gathering (Location interface) */

static gboolean
enable_disable_location_gathering_finish (MMIfaceModemLocation *self,
                                          GAsyncResult *res,
                                          GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void
gps_enabled_disabled_ready (MMBaseModem *self,
                            GAsyncResult *res,
                            GSimpleAsyncResult *simple)
{
    GError *error = NULL;

    if (!mm_base_modem_at_command_full_finish (self, res, &error))
        g_simple_async_result_take_error (simple, error);
    else
        g_simple_async_result_set_op_res_gboolean (simple, TRUE);

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
disable_location_gathering (MMIfaceModemLocation *self,
                            MMModemLocationSource source,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        disable_location_gathering);

    /* We only provide specific disabling for GPS-NMEA sources */
    if (source & MM_MODEM_LOCATION_SOURCE_GPS_NMEA) {
        /* We enable continuous GPS fixes with AT_OGPS=0 */
        mm_base_modem_at_command_full (MM_BASE_MODEM (self),
                                       mm_base_modem_peek_port_gps_control (MM_BASE_MODEM (self)),
                                       "_OGPS=0",
                                       3,
                                       FALSE,
                                       NULL, /* cancellable */
                                       (GAsyncReadyCallback)gps_enabled_disabled_ready,
                                       result);
        return;
    }

    /* For any other location (e.g. 3GPP), just return */
    g_simple_async_result_set_op_res_gboolean (result, TRUE);
    g_simple_async_result_complete_in_idle (result);
    g_object_unref (result);
}

static void
enable_location_gathering (MMIfaceModemLocation *self,
                           MMModemLocationSource source,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        enable_location_gathering);

    /* We only provide specific enabling for GPS-NMEA sources */
    if (source & MM_MODEM_LOCATION_SOURCE_GPS_NMEA) {
        /* We enable continuous GPS fixes with AT_OGPS=2 */
        mm_base_modem_at_command_full (MM_BASE_MODEM (self),
                                       mm_base_modem_peek_port_gps_control (MM_BASE_MODEM (self)),
                                       "_OGPS=2",
                                       3,
                                       FALSE,
                                       NULL, /* cancellable */
                                       (GAsyncReadyCallback)gps_enabled_disabled_ready,
                                       result);
        return;
    }

    /* For any other location (e.g. 3GPP), just return */
    g_simple_async_result_set_op_res_gboolean (result, TRUE);
    g_simple_async_result_complete_in_idle (result);
    g_object_unref (result);
}

/*****************************************************************************/
/* Setup ports (Broadband modem class) */

static void
setup_ports (MMBroadbandModem *self)
{
    /* Call parent's setup ports first always */
    MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_hso_parent_class)->setup_ports (self);

    /* _OWANCALL unsolicited messages are only expected in the primary port. */
    mm_at_serial_port_add_unsolicited_msg_handler (
        mm_base_modem_peek_port_primary (MM_BASE_MODEM (self)),
        MM_BROADBAND_MODEM_HSO (self)->priv->_owancall_regex,
        NULL, NULL, NULL);

    g_object_set (mm_base_modem_peek_port_primary (MM_BASE_MODEM (self)),
                  MM_SERIAL_PORT_SEND_DELAY, (guint64) 0,
                  /* built-in echo removal conflicts with unsolicited _OWANCALL
                   * messages, which are not <CR><LF> prefixed. */
                  MM_AT_SERIAL_PORT_REMOVE_ECHO, FALSE,
                  NULL);
}

/*****************************************************************************/

MMBroadbandModemHso *
mm_broadband_modem_hso_new (const gchar *device,
                              const gchar *driver,
                              const gchar *plugin,
                              guint16 vendor_id,
                              guint16 product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_HSO,
                         MM_BASE_MODEM_DEVICE, device,
                         MM_BASE_MODEM_DRIVER, driver,
                         MM_BASE_MODEM_PLUGIN, plugin,
                         MM_BASE_MODEM_VENDOR_ID, vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         NULL);
}

static void
finalize (GObject *object)
{
    MMBroadbandModemHso *self = MM_BROADBAND_MODEM_HSO (object);

    g_regex_unref (self->priv->_owancall_regex);

    G_OBJECT_CLASS (mm_broadband_modem_hso_parent_class)->finalize (object);
}

static void
mm_broadband_modem_hso_init (MMBroadbandModemHso *self)
{
    /* Initialize private data */
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE ((self),
                                              MM_TYPE_BROADBAND_MODEM_HSO,
                                              MMBroadbandModemHsoPrivate);

    self->priv->_owancall_regex = g_regex_new ("_OWANCALL: (\\d),\\s*(\\d)\\r\\n",
                                               G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
}

static void
iface_modem_init (MMIfaceModem *iface)
{
    iface->create_bearer = modem_create_bearer;
    iface->create_bearer_finish = modem_create_bearer_finish;

    /* HSO modems don't need the extra 10s wait after powering up */
    iface->modem_after_power_up = NULL;
    iface->modem_after_power_up_finish = NULL;
}

static void
iface_modem_3gpp_init (MMIfaceModem3gpp *iface)
{
    iface_modem_3gpp_parent = g_type_interface_peek_parent (iface);

    iface->setup_unsolicited_events = modem_3gpp_setup_unsolicited_events;
    iface->setup_unsolicited_events_finish = modem_3gpp_setup_cleanup_unsolicited_events_finish;
    iface->cleanup_unsolicited_events = modem_3gpp_cleanup_unsolicited_events;
    iface->cleanup_unsolicited_events_finish = modem_3gpp_setup_cleanup_unsolicited_events_finish;
}

static void
iface_modem_location_init (MMIfaceModemLocation *iface)
{
    iface_modem_location_parent = g_type_interface_peek_parent (iface);

    iface->load_capabilities = location_load_capabilities;
    iface->load_capabilities_finish = location_load_capabilities_finish;
    iface->enable_location_gathering = enable_location_gathering;
    iface->enable_location_gathering_finish = enable_disable_location_gathering_finish;
    iface->disable_location_gathering = disable_location_gathering;
    iface->disable_location_gathering_finish = enable_disable_location_gathering_finish;
}

static void
mm_broadband_modem_hso_class_init (MMBroadbandModemHsoClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    MMBroadbandModemClass *broadband_modem_class = MM_BROADBAND_MODEM_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMBroadbandModemHsoPrivate));

    object_class->finalize = finalize;
    broadband_modem_class->setup_ports = setup_ports;
}