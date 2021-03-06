/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2008 - 2009 Novell, Inc.
 * Copyright (C) 2009 - 2010 Red Hat, Inc.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define G_UDEV_API_IS_SUBJECT_TO_CHANGE
#include <gudev/gudev.h>

#include "mm-modem-huawei-gsm.h"
#include "mm-modem-gsm-network.h"
#include "mm-modem-gsm-card.h"
#include "mm-errors.h"
#include "mm-callback-info.h"
#include "mm-at-serial-port.h"
#include "mm-serial-parsers.h"

static void modem_init (MMModem *modem_class);
static void modem_gsm_network_init (MMModemGsmNetwork *gsm_network_class);
static void modem_gsm_card_init (MMModemGsmCard *gsm_card_class);

G_DEFINE_TYPE_EXTENDED (MMModemHuaweiGsm, mm_modem_huawei_gsm, MM_TYPE_GENERIC_GSM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_MODEM, modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_MODEM_GSM_NETWORK, modem_gsm_network_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_MODEM_GSM_CARD, modem_gsm_card_init))


#define MM_MODEM_HUAWEI_GSM_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), MM_TYPE_MODEM_HUAWEI_GSM, MMModemHuaweiGsmPrivate))

typedef struct {
    /* Cached state */
    guint32 band;
} MMModemHuaweiGsmPrivate;

MMModem *
mm_modem_huawei_gsm_new (const char *device,
                         const char *driver,
                         const char *plugin)
{
    g_return_val_if_fail (device != NULL, NULL);
    g_return_val_if_fail (driver != NULL, NULL);
    g_return_val_if_fail (plugin != NULL, NULL);

    return MM_MODEM (g_object_new (MM_TYPE_MODEM_HUAWEI_GSM,
                                   MM_MODEM_MASTER_DEVICE, device,
                                   MM_MODEM_DRIVER, driver,
                                   MM_MODEM_PLUGIN, plugin,
                                   NULL));
}

/*****************************************************************************/

typedef struct {
    MMModemGsmBand mm;
    guint32 huawei;
} BandTable;

static BandTable bands[] = {
    /* Sort 3G first since it's preferred */
    { MM_MODEM_GSM_BAND_U2100, 0x00400000 },
    { MM_MODEM_GSM_BAND_U1900, 0x00800000 },
    { MM_MODEM_GSM_BAND_U850,  0x04000000 },
    { MM_MODEM_GSM_BAND_U900,  0x00020000 },
    { MM_MODEM_GSM_BAND_G850,  0x00080000 },
    /* 2G second */
    { MM_MODEM_GSM_BAND_DCS,   0x00000080 },
    { MM_MODEM_GSM_BAND_EGSM,  0x00000300 }, /* 0x100 = Extended GSM, 0x200 = Primary GSM */
    { MM_MODEM_GSM_BAND_PCS,   0x00200000 },
    /* And ANY last since it's most inclusive */
    { MM_MODEM_GSM_BAND_ANY,   0x3FFFFFFF },
};

static gboolean
band_mm_to_huawei (MMModemGsmBand band, guint32 *out_huawei)
{
    int i;

    for (i = 0; i < sizeof (bands) / sizeof (BandTable); i++) {
        if (bands[i].mm == band) {
            *out_huawei = bands[i].huawei;
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
band_huawei_to_mm (guint32 huawei, MMModemGsmBand *out_mm)
{
    int i;

    for (i = 0; i < sizeof (bands) / sizeof (BandTable); i++) {
        /* The dongle returns a bitfield, but since we don't support that
         * yet in MM, take the "best" band and return it.
         */
        if (bands[i].huawei & huawei) {
            *out_mm = bands[i].mm;
            return TRUE;
        }
    }
    return FALSE;
}

/*****************************************************************************/

static gboolean
parse_syscfg (MMModemHuaweiGsm *self,
              const char *reply,
              int *mode_a,
              int *mode_b,
              guint32 *band,
              int *unknown1,
              int *unknown2,
              MMModemGsmAllowedMode *out_mode)
{
    if (reply == NULL || strncmp (reply, "^SYSCFG:", 8))
        return FALSE;

    if (sscanf (reply + 8, "%d,%d,%x,%d,%d", mode_a, mode_b, band, unknown1, unknown2)) {
        MMModemHuaweiGsmPrivate *priv = MM_MODEM_HUAWEI_GSM_GET_PRIVATE (self);
        MMModemGsmAllowedMode new_mode = MM_MODEM_GSM_ALLOWED_MODE_ANY;

        /* Network mode */
        if (*mode_a == 2 && *mode_b == 1)
            new_mode = MM_MODEM_GSM_ALLOWED_MODE_2G_PREFERRED;
        else if (*mode_a == 2 && *mode_b == 2)
            new_mode = MM_MODEM_GSM_ALLOWED_MODE_3G_PREFERRED;
        else if (*mode_a == 13 && *mode_b == 1)
            new_mode = MM_MODEM_GSM_ALLOWED_MODE_2G_ONLY;
        else if (*mode_a == 14 && *mode_b == 2)
            new_mode = MM_MODEM_GSM_ALLOWED_MODE_3G_ONLY;

        if (out_mode)
            *out_mode = new_mode;

        /* Band */
        priv->band = *band;

        return TRUE;
    }

    return FALSE;
}

static void
set_allowed_mode_done (MMAtSerialPort *port,
                       GString *response,
                       GError *error,
                       gpointer user_data)
{
    MMCallbackInfo *info = (MMCallbackInfo *) user_data;

    if (error)
        info->error = g_error_copy (error);

    mm_callback_info_schedule (info);
}

static void
set_allowed_mode (MMGenericGsm *gsm,
                  MMModemGsmAllowedMode mode,
                  MMModemFn callback,
                  gpointer user_data)
{
    MMCallbackInfo *info;
    MMAtSerialPort *port;
    int a, b;
    char *command;

    info = mm_callback_info_new (MM_MODEM (gsm), callback, user_data);

    port = mm_generic_gsm_get_best_at_port (gsm, &info->error);
    if (!port) {
        mm_callback_info_schedule (info);
        return;
    }

    switch (mode) {
    case MM_MODEM_GSM_ALLOWED_MODE_2G_ONLY:
        a = 13;
        b = 1;
        break;
    case MM_MODEM_GSM_ALLOWED_MODE_3G_ONLY:
        a = 14;
        b = 2;
        break;
    case MM_MODEM_GSM_ALLOWED_MODE_2G_PREFERRED:
        a = 2;
        b = 1;
        break;
    case MM_MODEM_GSM_ALLOWED_MODE_3G_PREFERRED:
        a = 2;
        b = 2;
        break;
    case MM_MODEM_GSM_ALLOWED_MODE_ANY:
    default:
        a = 2;
        b = 0;
        break;
    }

    command = g_strdup_printf ("AT^SYSCFG=%d,%d,40000000,2,4", a, b);
    mm_at_serial_port_queue_command (port, command, 3, set_allowed_mode_done, info);
    g_free (command);
}

static void
get_allowed_mode_done (MMAtSerialPort *port,
                       GString *response,
                       GError *error,
                       gpointer user_data)
{
    MMCallbackInfo *info = (MMCallbackInfo *) user_data;
    MMModemHuaweiGsm *self = MM_MODEM_HUAWEI_GSM (info->modem);
    int mode_a, mode_b, u1, u2;
    guint32 band;
    MMModemGsmAllowedMode mode = MM_MODEM_GSM_ALLOWED_MODE_ANY;

    if (error)
        info->error = g_error_copy (error);
    else if (parse_syscfg (self, response->str, &mode_a, &mode_b, &band, &u1, &u2, &mode))
        mm_callback_info_set_result (info, GUINT_TO_POINTER (mode), NULL);

    mm_callback_info_schedule (info);
}

static void
get_allowed_mode (MMGenericGsm *gsm,
                  MMModemUIntFn callback,
                  gpointer user_data)
{
    MMCallbackInfo *info;
    MMAtSerialPort *port;

    info = mm_callback_info_uint_new (MM_MODEM (gsm), callback, user_data);

    port = mm_generic_gsm_get_best_at_port (gsm, &info->error);
    if (!port) {
        mm_callback_info_schedule (info);
        return;
    }

    mm_at_serial_port_queue_command (port, "AT^SYSCFG?", 3, get_allowed_mode_done, info);
}

static void
set_band_done (MMAtSerialPort *port,
               GString *response,
               GError *error,
               gpointer user_data)
{
    MMCallbackInfo *info = (MMCallbackInfo *) user_data;
    MMModemHuaweiGsm *self = MM_MODEM_HUAWEI_GSM (info->modem);
    MMModemHuaweiGsmPrivate *priv = MM_MODEM_HUAWEI_GSM_GET_PRIVATE (self);

    if (error)
        info->error = g_error_copy (error);
    else {
        /* Success, cache the value */
        priv->band = GPOINTER_TO_UINT (mm_callback_info_get_data (info, "band"));
    }

    mm_callback_info_schedule (info);
}

static void
set_band (MMModemGsmNetwork *modem,
          MMModemGsmBand band,
          MMModemFn callback,
          gpointer user_data)
{
    MMCallbackInfo *info;
    MMAtSerialPort *port;
    char *command;
    guint32 huawei_band = 0x3FFFFFFF;

    info = mm_callback_info_new (MM_MODEM (modem), callback, user_data);

    port = mm_generic_gsm_get_best_at_port (MM_GENERIC_GSM (modem), &info->error);
    if (!port) {
        mm_callback_info_schedule (info);
        return;
    }

    if (!band_mm_to_huawei (band, &huawei_band)) {
        info->error = g_error_new_literal (MM_MODEM_ERROR, MM_MODEM_ERROR_GENERAL, "Invalid band.");
        mm_callback_info_schedule (info);
    } else {
        mm_callback_info_set_data (info, "band", GUINT_TO_POINTER (huawei_band), NULL);
        command = g_strdup_printf ("AT^SYSCFG=16,3,%X,2,4", huawei_band);
        mm_at_serial_port_queue_command (port, command, 3, set_band_done, info);
        g_free (command);
    }
}

static void
get_band_done (MMAtSerialPort *port,
               GString *response,
               GError *error,
               gpointer user_data)
{
    MMCallbackInfo *info = (MMCallbackInfo *) user_data;
    MMModemHuaweiGsm *self = MM_MODEM_HUAWEI_GSM (info->modem);
    int mode_a, mode_b, u1, u2;
    guint32 band;

    if (error)
        info->error = g_error_copy (error);
    else if (parse_syscfg (self, response->str, &mode_a, &mode_b, &band, &u1, &u2, NULL)) {
        MMModemGsmBand mm_band = MM_MODEM_GSM_BAND_ANY;

        band_huawei_to_mm (band, &mm_band);
        mm_callback_info_set_result (info, GUINT_TO_POINTER (mm_band), NULL);
    }

    mm_callback_info_schedule (info);
}

static void
get_band (MMModemGsmNetwork *modem,
          MMModemUIntFn callback,
          gpointer user_data)
{
    MMModemHuaweiGsmPrivate *priv = MM_MODEM_HUAWEI_GSM_GET_PRIVATE (modem);
    MMAtSerialPort *port;
    MMCallbackInfo *info;

    info = mm_callback_info_uint_new (MM_MODEM (modem), callback, user_data);

    /* Prefer cached band from unsolicited messages if we have it */
    if (priv->band != 0) {
        MMModemGsmBand mm_band = MM_MODEM_GSM_BAND_ANY;

        band_huawei_to_mm (priv->band, &mm_band);
        mm_callback_info_set_result (info, GUINT_TO_POINTER (mm_band), NULL);
        mm_callback_info_schedule (info);
        return;
    }

    /* Otherwise ask the modem */
    port = mm_generic_gsm_get_best_at_port (MM_GENERIC_GSM (modem), &info->error);
    if (!port) {
        mm_callback_info_schedule (info);
        return;
    }

    mm_at_serial_port_queue_command (port, "AT^SYSCFG?", 3, get_band_done, info);
}

static void
get_act_request_done (MMAtSerialPort *port,
                      GString *response,
                      GError *error,
                      gpointer user_data)
{
    MMCallbackInfo *info = user_data;
    MMModemGsmAccessTech act = MM_MODEM_GSM_ACCESS_TECH_UNKNOWN;
    GRegex *r = NULL;
    GMatchInfo *match_info = NULL;
    char *str;
    int srv_stat = 0;

    if (error) {
        info->error = g_error_copy (error);
        goto done;
    }

    /* Can't just use \d here since sometimes you get "^SYSINFO:2,1,0,3,1,,3" */
    r = g_regex_new ("\\^SYSINFO:\\s*(\\d?),(\\d?),(\\d?),(\\d?),(\\d?),(\\d?),(\\d?)$", G_REGEX_UNGREEDY, 0, NULL);
    if (!r) {
        g_set_error_literal (&info->error,
                             MM_MODEM_ERROR, MM_MODEM_ERROR_GENERAL,
                             "Could not parse ^SYSINFO results.");
        goto done;
    }

    if (!g_regex_match_full (r, response->str, response->len, 0, 0, &match_info, &info->error)) {
        g_set_error_literal (&info->error,
                             MM_MODEM_ERROR, MM_MODEM_ERROR_GENERAL,
                             "Could not parse ^SYSINFO results.");
        goto done;
    }

    str = g_match_info_fetch (match_info, 1);
    if (str && strlen (str))
        srv_stat = atoi (str);
    g_free (str);

    if (srv_stat != 0) {
        /* Valid service */
        str = g_match_info_fetch (match_info, 7);
        if (str && strlen (str)) {
            if (str[0] == '1')
                act = MM_MODEM_GSM_ACCESS_TECH_GSM;
            else if (str[0] == '2')
                act = MM_MODEM_GSM_ACCESS_TECH_GPRS;
            else if (str[0] == '3')
                act = MM_MODEM_GSM_ACCESS_TECH_EDGE;
            else if (str[0] == '4')
                act = MM_MODEM_GSM_ACCESS_TECH_UMTS;
            else if (str[0] == '5')
                act = MM_MODEM_GSM_ACCESS_TECH_HSDPA;
            else if (str[0] == '6')
                act = MM_MODEM_GSM_ACCESS_TECH_HSUPA;
            else if (str[0] == '7')
                act = MM_MODEM_GSM_ACCESS_TECH_HSPA;
        }
        g_free (str);
    }

done:
    if (match_info)
        g_match_info_free (match_info);
    if (r)
        g_regex_unref (r);
    mm_callback_info_set_result (info, GUINT_TO_POINTER (act), NULL);
    mm_callback_info_schedule (info);
}

static void
get_access_technology (MMGenericGsm *modem,
                       MMModemUIntFn callback,
                       gpointer user_data)
{
    MMAtSerialPort *port;
    MMCallbackInfo *info;

    info = mm_callback_info_uint_new (MM_MODEM (modem), callback, user_data);

    port = mm_generic_gsm_get_best_at_port (modem, &info->error);
    if (!port) {
        mm_callback_info_schedule (info);
        return;
    }

    mm_at_serial_port_queue_command (port, "^SYSINFO", 3, get_act_request_done, info);
}

/*****************************************************************************/

static gboolean
parse_num (const char *str, guint32 *out_num, guint32 min, guint32 max)
{
    unsigned long int tmp;

    if (!str || !strlen (str))
        return FALSE;

    errno = 0;
    tmp = strtoul (str, NULL, 10);
    if (errno != 0 || tmp < min || tmp > max)
        return FALSE;
    *out_num = (guint32) tmp;
    return TRUE;
}

static void
send_huawei_cpin_done (MMAtSerialPort *port,
                       GString *response,
                       GError *error,
                       gpointer user_data)
{
    MMCallbackInfo *info = (MMCallbackInfo *) user_data;
    GRegex *r = NULL;
    GMatchInfo *match_info = NULL;
    const char *pin_type;
    guint32 attempts_left = 0;
    char *str = NULL;
    guint32 num = 0;

    if (error) {
        info->error = g_error_copy (error);
        goto done;
    }

    pin_type = mm_callback_info_get_data (info, "pin_type");

	r = g_regex_new ("\\^CPIN:\\s*([^,]+),[^,]*,(\\d+),(\\d+),(\\d+),(\\d+)", G_REGEX_UNGREEDY, 0, NULL);
    if (!r) {
        g_set_error_literal (&info->error,
                             MM_MODEM_ERROR, MM_MODEM_ERROR_GENERAL,
                             "Could not parse ^CPIN results (error creating regex).");
        goto done;
    }

    if (!g_regex_match_full (r, response->str, response->len, 0, 0, &match_info, &info->error)) {
        g_set_error_literal (&info->error,
                             MM_MODEM_ERROR, MM_MODEM_ERROR_GENERAL,
                             "Could not parse ^CPIN results (match failed).");
        goto done;
    }

    if (strstr (pin_type, MM_MODEM_GSM_CARD_SIM_PUK))
        num = 2;
    else if (strstr (pin_type, MM_MODEM_GSM_CARD_SIM_PIN))
        num = 3;
    else if (strstr (pin_type, MM_MODEM_GSM_CARD_SIM_PUK2))
        num = 4;
    else if (strstr (pin_type, MM_MODEM_GSM_CARD_SIM_PIN2))
        num = 5;
    else {
        g_debug ("%s: unhandled pin type '%s'", __func__, pin_type);

        info->error = g_error_new_literal (MM_MODEM_ERROR, MM_MODEM_ERROR_GENERAL, "Unhandled PIN type");
    }

    if (num > 0) {
        gboolean success = FALSE;

        str = g_match_info_fetch (match_info, num);
        if (str) {
            success = parse_num (str, &attempts_left, 0, 10);
            g_free (str);
        }

        if (!success) {
            info->error = g_error_new_literal (MM_MODEM_ERROR,
                                               MM_MODEM_ERROR_GENERAL,
                                               "Could not parse ^CPIN results (missing or invalid match info).");
        }
    }

    mm_callback_info_set_result (info, GUINT_TO_POINTER (attempts_left), NULL);

    g_match_info_free (match_info);

done:
    if (r)
        g_regex_unref (r);
    mm_serial_port_close (MM_SERIAL_PORT (port));
    mm_callback_info_schedule (info);
}

static void
get_unlock_retries (MMModemGsmCard *modem,
                    const char *pin_type,
                    MMModemUIntFn callback,
                    gpointer user_data)
{
    MMAtSerialPort *port;
    char *command;
    MMCallbackInfo *info = mm_callback_info_uint_new (MM_MODEM (modem), callback, user_data);

    g_debug ("%s: pin type '%s'", __func__, pin_type);

    /* Ensure we have a usable port to use for the command */
    port = mm_generic_gsm_get_best_at_port (MM_GENERIC_GSM (modem), &info->error);
    if (!port) {
        mm_callback_info_schedule (info);
        return;
    }

    /* Modem may not be enabled yet, which sometimes can't be done until
     * the device has been unlocked.  In this case we have to open the port
     * ourselves.
     */
    if (!mm_serial_port_open (MM_SERIAL_PORT (port), &info->error)) {
        mm_callback_info_schedule (info);
        return;
    }

    /* if the modem have not yet been enabled we need to make sure echoing is turned off */
    command = g_strdup_printf ("E0");
    mm_at_serial_port_queue_command (port, command, 3, NULL, NULL);
    g_free (command);

    mm_callback_info_set_data (info, "pin_type", g_strdup (pin_type), g_free);

    command = g_strdup_printf ("^CPIN?");
    mm_at_serial_port_queue_command (port, command, 3, send_huawei_cpin_done, info);
    g_free (command);
}

/*****************************************************************************/
/* Unsolicited message handlers */

static void
handle_signal_quality_change (MMAtSerialPort *port,
                              GMatchInfo *match_info,
                              gpointer user_data)
{
    MMModemHuaweiGsm *self = MM_MODEM_HUAWEI_GSM (user_data);
    char *str;
    int quality = 0;

    str = g_match_info_fetch (match_info, 1);
    quality = atoi (str);
    g_free (str);

    if (quality == 99) {
        /* 99 means unknown */
        quality = 0;
    } else {
        /* Normalize the quality */
        quality = CLAMP (quality, 0, 31) * 100 / 31;
    }

    mm_generic_gsm_update_signal_quality (MM_GENERIC_GSM (self), (guint32) quality);
}

static void
handle_mode_change (MMAtSerialPort *port,
                    GMatchInfo *match_info,
                    gpointer user_data)
{
    MMModemHuaweiGsm *self = MM_MODEM_HUAWEI_GSM (user_data);
    MMModemGsmAccessTech act = MM_MODEM_GSM_ACCESS_TECH_UNKNOWN;
    char *str;
    int a;
    int b;

    str = g_match_info_fetch (match_info, 1);
    a = atoi (str);
    g_free (str);

    str = g_match_info_fetch (match_info, 2);
    b = atoi (str);
    g_free (str);

    if (a == 3) {   /* GSM/GPRS mode */
        if (b == 1)
            act = MM_MODEM_GSM_ACCESS_TECH_GSM;
        else if (b == 2)
            act = MM_MODEM_GSM_ACCESS_TECH_GPRS;
        else if (b == 3)
            act = MM_MODEM_GSM_ACCESS_TECH_EDGE;
    } else if (a == 5) {  /* WCDMA mode */
        if (b == 4)
            act = MM_MODEM_GSM_ACCESS_TECH_UMTS;
        else if (b == 5)
            act = MM_MODEM_GSM_ACCESS_TECH_HSDPA;
        else if (b == 6)
            act = MM_MODEM_GSM_ACCESS_TECH_HSUPA;
        else if (b == 7)
            act = MM_MODEM_GSM_ACCESS_TECH_HSPA;
    } else if (a == 0)
        act = MM_MODEM_GSM_ACCESS_TECH_UNKNOWN;
    else {
        g_warning ("Couldn't parse mode change value: '%s'", str);
        return;
    }

    g_debug ("Access Technology: %d", act);
    mm_generic_gsm_update_access_technology (MM_GENERIC_GSM (self), act);
}

static void
handle_status_change (MMAtSerialPort *port,
                      GMatchInfo *match_info,
                      gpointer user_data)
{
    char *str;
    int n1, n2, n3, n4, n5, n6, n7;

    str = g_match_info_fetch (match_info, 1);
    if (sscanf (str, "%x,%x,%x,%x,%x,%x,%x", &n1, &n2, &n3, &n4, &n5, &n6, &n7)) {
        g_debug ("Duration: %d Up: %d Kbps Down: %d Kbps Total: %d Total: %d\n",
                 n1, n2 * 8 / 1000, n3  * 8 / 1000, n4 / 1024, n5 / 1024);
    }
    g_free (str);
}

/*****************************************************************************/

static gboolean
grab_port (MMModem *modem,
           const char *subsys,
           const char *name,
           MMPortType suggested_type,
           gpointer user_data,
           GError **error)
{
    MMGenericGsm *gsm = MM_GENERIC_GSM (modem);
    MMPortType ptype = MM_PORT_TYPE_IGNORED;
    const char *sys[] = { "tty", NULL };
    GUdevClient *client;
    GUdevDevice *device = NULL;
    MMPort *port = NULL;
    int usbif;

    client = g_udev_client_new (sys);
    if (!client) {
        g_set_error (error, 0, 0, "Could not get udev client.");
        return FALSE;
    }

    device = g_udev_client_query_by_subsystem_and_name (client, subsys, name);
    if (!device) {
        g_set_error (error, 0, 0, "Could not get udev device.");
        goto out;
    }

    usbif = g_udev_device_get_property_as_int (device, "ID_USB_INTERFACE_NUM");
    if (usbif < 0) {
        g_set_error (error, 0, 0, "Could not get USB device interface number.");
        goto out;
    }

    if (usbif == 0) {
        if (!mm_generic_gsm_get_at_port (gsm, MM_PORT_TYPE_PRIMARY))
            ptype = MM_PORT_TYPE_PRIMARY;
    } else if (suggested_type == MM_PORT_TYPE_SECONDARY) {
        if (!mm_generic_gsm_get_at_port (gsm, MM_PORT_TYPE_SECONDARY))
            ptype = MM_PORT_TYPE_SECONDARY;
    }

    port = mm_generic_gsm_grab_port (gsm, subsys, name, ptype, error);

    if (port && MM_IS_AT_SERIAL_PORT (port)) {
        GRegex *regex;

        g_object_set (G_OBJECT (port), MM_PORT_CARRIER_DETECT, FALSE, NULL);

        regex = g_regex_new ("\\r\\n\\^RSSI:(\\d+)\\r\\n", G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
        mm_at_serial_port_add_unsolicited_msg_handler (MM_AT_SERIAL_PORT (port), regex, handle_signal_quality_change, modem, NULL);
        g_regex_unref (regex);

        regex = g_regex_new ("\\r\\n\\^MODE:(\\d),(\\d)\\r\\n", G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
        mm_at_serial_port_add_unsolicited_msg_handler (MM_AT_SERIAL_PORT (port), regex, handle_mode_change, modem, NULL);
        g_regex_unref (regex);

        regex = g_regex_new ("\\r\\n\\^DSFLOWRPT:(.+)\\r\\n", G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
        mm_at_serial_port_add_unsolicited_msg_handler (MM_AT_SERIAL_PORT (port), regex, handle_status_change, modem, NULL);
        g_regex_unref (regex);

        regex = g_regex_new ("\\r\\n\\^BOOT:.+\\r\\n", G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
        mm_at_serial_port_add_unsolicited_msg_handler (MM_AT_SERIAL_PORT (port), regex, NULL, modem, NULL);
        g_regex_unref (regex);
    }

out:
    if (device)
        g_object_unref (device);
    g_object_unref (client);
    return !!port;
}

/*****************************************************************************/

static void
modem_init (MMModem *modem_class)
{
    modem_class->grab_port = grab_port;
}

static void
modem_gsm_network_init (MMModemGsmNetwork *class)
{
    class->set_band = set_band;
    class->get_band = get_band;
}

static void
modem_gsm_card_init (MMModemGsmCard *class)
{
    class->get_unlock_retries = get_unlock_retries;
}

static void
mm_modem_huawei_gsm_init (MMModemHuaweiGsm *self)
{
}

static void
mm_modem_huawei_gsm_class_init (MMModemHuaweiGsmClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    MMGenericGsmClass *gsm_class = MM_GENERIC_GSM_CLASS (klass);

    mm_modem_huawei_gsm_parent_class = g_type_class_peek_parent (klass);
    g_type_class_add_private (object_class, sizeof (MMModemHuaweiGsmPrivate));

    gsm_class->set_allowed_mode = set_allowed_mode;
    gsm_class->get_allowed_mode = get_allowed_mode;
    gsm_class->get_access_technology = get_access_technology;
}

