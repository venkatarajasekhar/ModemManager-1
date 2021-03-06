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
 * Copyright (C) 2013 Huawei Technologies Co., Ltd
 * Copyright (C) 2013 Aleksander Morgado <aleksander@gnu.org>
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-log.h"
#include "mm-modem-helpers.h"
#include "mm-modem-helpers-huawei.h"

/*****************************************************************************/
/* ^NDISSTAT /  ^NDISSTATQRY response parser */

gboolean
mm_huawei_parse_ndisstatqry_response (const gchar *response,
                                      gboolean *ipv4_available,
                                      gboolean *ipv4_connected,
                                      gboolean *ipv6_available,
                                      gboolean *ipv6_connected,
                                      GError **error)
{
    GRegex *r;
    GMatchInfo *match_info;
    GError *inner_error = NULL;

    if (!response ||
        !(g_str_has_prefix (response, "^NDISSTAT:") ||
          g_str_has_prefix (response, "^NDISSTATQRY:"))) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "Missing ^NDISSTAT / ^NDISSTATQRY prefix");
        return FALSE;
    }

    *ipv4_available = FALSE;
    *ipv6_available = FALSE;

    /* The response maybe as:
     *     ^NDISSTAT: 1,,,IPV4
     *     ^NDISSTAT: 0,33,,IPV6
     *     ^NDISSTATQRY: 1,,,IPV4
     *     ^NDISSTATQRY: 0,33,,IPV6
     *     OK
     *
     * Or, in newer firmwares:
     *     ^NDISSTATQRY:0,,,"IPV4",0,,,"IPV6"
     *     OK
     */
    r = g_regex_new ("\\^NDISSTAT(?:QRY)?:\\s*(\\d),([^,]*),([^,]*),([^,\\r\\n]*)(?:\\r\\n)?"
                     "(?:\\^NDISSTAT:|\\^NDISSTATQRY:)?\\s*,?(\\d)?,?([^,]*)?,?([^,]*)?,?([^,\\r\\n]*)?(?:\\r\\n)?",
                     G_REGEX_DOLLAR_ENDONLY | G_REGEX_RAW,
                     0, NULL);
    g_assert (r != NULL);

    g_regex_match_full (r, response, strlen (response), 0, 0, &match_info, &inner_error);
    if (!inner_error && g_match_info_matches (match_info)) {
        guint ip_type_field = 4;

        /* IPv4 and IPv6 are fields 4 and (if available) 8 */

        while (!inner_error && ip_type_field <= 8) {
            gchar *ip_type_str;
            guint connected;

            ip_type_str = mm_get_string_unquoted_from_match_info (match_info, ip_type_field);
            if (!ip_type_str)
                break;

            if (!mm_get_uint_from_match_info (match_info, (ip_type_field - 3), &connected) ||
                (connected != 0 && connected != 1)) {
                inner_error = g_error_new (MM_CORE_ERROR,
                                           MM_CORE_ERROR_FAILED,
                                           "Couldn't parse ^NDISSTAT / ^NDISSTATQRY fields");
            } else if (g_ascii_strcasecmp (ip_type_str, "IPV4") == 0) {
                *ipv4_available = TRUE;
                *ipv4_connected = (gboolean)connected;
            } else if (g_ascii_strcasecmp (ip_type_str, "IPV6") == 0) {
                *ipv6_available = TRUE;
                *ipv6_connected = (gboolean)connected;
            }

            g_free (ip_type_str);
            ip_type_field += 4;
        }
    }

    g_match_info_free (match_info);
    g_regex_unref (r);

    if (!ipv4_available && !ipv6_available) {
        inner_error = g_error_new (MM_CORE_ERROR,
                                   MM_CORE_ERROR_FAILED,
                                   "Couldn't find IPv4 or IPv6 info in ^NDISSTAT / ^NDISSTATQRY response");
    }

    if (inner_error) {
        g_propagate_error (error, inner_error);
        return FALSE;
    }

    return TRUE;
}

/*****************************************************************************/
/* ^SYSINFO response parser */

gboolean
mm_huawei_parse_sysinfo_response (const char *reply,
                                  guint *out_srv_status,
                                  guint *out_srv_domain,
                                  guint *out_roam_status,
                                  guint *out_sys_mode,
                                  guint *out_sim_state,
                                  gboolean *out_sys_submode_valid,
                                  guint *out_sys_submode,
                                  GError **error)
{
    gboolean matched;
    GRegex *r;
    GMatchInfo *match_info = NULL;
    GError *match_error = NULL;

    g_assert (out_srv_status != NULL);
    g_assert (out_srv_domain != NULL);
    g_assert (out_roam_status != NULL);
    g_assert (out_sys_mode != NULL);
    g_assert (out_sim_state != NULL);
    g_assert (out_sys_submode_valid != NULL);
    g_assert (out_sys_submode != NULL);

    /* Format:
     *
     * ^SYSINFO: <srv_status>,<srv_domain>,<roam_status>,<sys_mode>,<sim_state>[,<reserved>,<sys_submode>]
     */

    /* Can't just use \d here since sometimes you get "^SYSINFO:2,1,0,3,1,,3" */
    r = g_regex_new ("\\^SYSINFO:\\s*(\\d+),(\\d+),(\\d+),(\\d+),(\\d+),?(\\d+)?,?(\\d+)?$", 0, 0, NULL);
    g_assert (r != NULL);

    matched = g_regex_match_full (r, reply, -1, 0, 0, &match_info, &match_error);
    if (!matched) {
        if (match_error) {
            g_propagate_error (error, match_error);
            g_prefix_error (error, "Could not parse ^SYSINFO results: ");
        } else {
            g_set_error_literal (error,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_FAILED,
                                 "Couldn't match ^SYSINFO reply");
        }
    } else {
        mm_get_uint_from_match_info (match_info, 1, out_srv_status);
        mm_get_uint_from_match_info (match_info, 2, out_srv_domain);
        mm_get_uint_from_match_info (match_info, 3, out_roam_status);
        mm_get_uint_from_match_info (match_info, 4, out_sys_mode);
        mm_get_uint_from_match_info (match_info, 5, out_sim_state);

        /* Remember that g_match_info_get_match_count() includes match #0 */
        if (g_match_info_get_match_count (match_info) >= 8) {
            *out_sys_submode_valid = TRUE;
            mm_get_uint_from_match_info (match_info, 7, out_sys_submode);
        }
    }

    if (match_info)
        g_match_info_free (match_info);
    g_regex_unref (r);
    return matched;
}

/*****************************************************************************/
/* ^SYSINFOEX response parser */

gboolean
mm_huawei_parse_sysinfoex_response (const char *reply,
                                    guint *out_srv_status,
                                    guint *out_srv_domain,
                                    guint *out_roam_status,
                                    guint *out_sim_state,
                                    guint *out_sys_mode,
                                    guint *out_sys_submode,
                                    GError **error)
{
    gboolean matched;
    GRegex *r;
    GMatchInfo *match_info = NULL;
    GError *match_error = NULL;

    g_assert (out_srv_status != NULL);
    g_assert (out_srv_domain != NULL);
    g_assert (out_roam_status != NULL);
    g_assert (out_sim_state != NULL);
    g_assert (out_sys_mode != NULL);
    g_assert (out_sys_submode != NULL);

    /* Format:
     *
     * ^SYSINFOEX: <srv_status>,<srv_domain>,<roam_status>,<sim_state>,<reserved>,<sysmode>,<sysmode_name>,<submode>,<submode_name>
     *
     * <sysmode_name> and <submode_name> may not be quoted on some Huawei modems (e.g. E303).
     */

    /* ^SYSINFOEX:2,3,0,1,,3,"WCDMA",41,"HSPA+" */

    r = g_regex_new ("\\^SYSINFOEX:\\s*(\\d+),(\\d+),(\\d+),(\\d+),?(\\d*),(\\d+),\"?([^\"]*)\"?,(\\d+),\"?([^\"]*)\"?$", 0, 0, NULL);
    g_assert (r != NULL);

    matched = g_regex_match_full (r, reply, -1, 0, 0, &match_info, &match_error);
    if (!matched) {
        if (match_error) {
            g_propagate_error (error, match_error);
            g_prefix_error (error, "Could not parse ^SYSINFOEX results: ");
        } else {
            g_set_error_literal (error,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_FAILED,
                                 "Couldn't match ^SYSINFOEX reply");
        }
    } else {
        mm_get_uint_from_match_info (match_info, 1, out_srv_status);
        mm_get_uint_from_match_info (match_info, 2, out_srv_domain);
        mm_get_uint_from_match_info (match_info, 3, out_roam_status);
        mm_get_uint_from_match_info (match_info, 4, out_sim_state);

        /* We just ignore the sysmode and submode name strings */
        mm_get_uint_from_match_info (match_info, 6, out_sys_mode);
        mm_get_uint_from_match_info (match_info, 8, out_sys_submode);
    }

    if (match_info)
        g_match_info_free (match_info);
    g_regex_unref (r);
    return matched;
}

/*****************************************************************************/
/* ^PREFMODE test parser
 *
 * AT^PREFMODE=?
 *   ^PREFMODE:(2,4,8)
 */

static gboolean
mode_from_prefmode (guint huawei_mode,
                    MMModemMode *modem_mode,
                    GError **error)
{
    g_assert (modem_mode != NULL);

    *modem_mode = MM_MODEM_MODE_NONE;
    switch (huawei_mode) {
    case 2:
        *modem_mode = MM_MODEM_MODE_2G;
        break;
    case 4:
        *modem_mode = MM_MODEM_MODE_3G;
        break;
    case 8:
        *modem_mode = (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G);
        break;
    default:
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "No translation from huawei prefmode '%u' to mode",
                     huawei_mode);
    }

    return *modem_mode != MM_MODEM_MODE_NONE ? TRUE : FALSE;
}

GArray *
mm_huawei_parse_prefmode_test (const gchar *response,
                               GError **error)
{
    gchar **split;
    guint i;
    MMModemMode all = MM_MODEM_MODE_NONE;
    GArray *out;

    response = mm_strip_tag (response, "^PREFMODE:");
    split = g_strsplit_set (response, " (,)\r\n", -1);
    if (!split) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Unexpected ^PREFMODE format output");
        return NULL;
    }

    out = g_array_sized_new (FALSE,
                             FALSE,
                             sizeof (MMHuaweiPrefmodeCombination),
                             3);
    for (i = 0; split[i]; i++) {
        guint val;
        MMModemMode preferred = MM_MODEM_MODE_NONE;
        GError *inner_error = NULL;
        MMHuaweiPrefmodeCombination combination;

        if (split[i][0] == '\0')
            continue;

        if (!mm_get_uint_from_str (split[i], &val)) {
            mm_dbg ("Error parsing ^PREFMODE value: %s", split[i]);
            continue;
        }

        if (!mode_from_prefmode (val, &preferred, &inner_error)) {
            mm_dbg ("Unhandled ^PREFMODE: %s", inner_error->message);
            g_error_free (inner_error);
            continue;
        }

        combination.prefmode = val;
        combination.allowed = MM_MODEM_MODE_NONE; /* reset it later */
        combination.preferred = preferred;

        all |= preferred;

        g_array_append_val (out, combination);
    }
    g_strfreev (split);

    /* No value */
    if (out->len == 0) {
        g_array_unref (out);
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "^PREFMODE response contains no valid values");
        return NULL;
    }

    /* Single value listed; PREFERRED=NONE... */
    if (out->len == 1) {
        MMHuaweiPrefmodeCombination *combination;

        combination = &g_array_index (out, MMHuaweiPrefmodeCombination, 0);
        combination->allowed = all;
        combination->preferred = MM_MODEM_MODE_NONE;
    } else {
        /* Multiple values, reset ALLOWED */
        for (i = 0; i < out->len; i++) {
            MMHuaweiPrefmodeCombination *combination;

            combination = &g_array_index (out, MMHuaweiPrefmodeCombination, i);
            combination->allowed = all;
            if (combination->preferred == all)
                combination->preferred = MM_MODEM_MODE_NONE;
        }
    }

    return out;
}

/*****************************************************************************/
/* ^PREFMODE response parser */

const MMHuaweiPrefmodeCombination *
mm_huawei_parse_prefmode_response (const gchar *response,
                                   const GArray *supported_mode_combinations,
                                   GError **error)
{
    gint mode;
    guint i;

    /* Format:
     *
     * ^PREFMODE: <mode>
     */

    response = mm_strip_tag (response, "^PREFMODE:");
    if (!sscanf (response, "%d", &mode)) {
        /* Dump error to upper layer */
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Unexpected PREFMODE response: '%s'",
                     response);
        return NULL;
    }

    /* Look for current modes among the supported ones */
    for (i = 0; i < supported_mode_combinations->len; i++) {
        const MMHuaweiPrefmodeCombination *combination;

        combination = &g_array_index (supported_mode_combinations,
                                      MMHuaweiPrefmodeCombination,
                                      i);
        if (mode == combination->prefmode)
            return combination;
    }

    g_set_error (error,
                 MM_CORE_ERROR,
                 MM_CORE_ERROR_FAILED,
                 "No PREFMODE combination found matching the current one (%d)",
                 mode);
    return NULL;
}

/*****************************************************************************/
/* ^SYSCFG test parser */

static gchar **
split_groups (const gchar *str,
              GError **error)
{
    const gchar *p = str;
    GPtrArray *out;
    guint groups = 0;

    /*
     * Split string: (a),((b1),(b2)),,(d),((e1),(e2))
     * Into:
     *   - a
     *   - (b1),(b2)
     *   -
     *   - d
     *   - (e1),(e2)
     */

    out = g_ptr_array_new_with_free_func (g_free);

    while (TRUE) {
        const gchar *start;
        guint inner_groups;

        /* Skip whitespaces */
        while (*p == ' ' || *p == '\r' || *p == '\n')
            p++;

        /* We're done, return */
        if (*p == '\0') {
            g_ptr_array_set_size (out, out->len + 1);
            return (gchar **) g_ptr_array_free (out, FALSE);
        }

        /* Group separators */
        if (groups > 0) {
            if (*p != ',') {
                g_set_error (error,
                             MM_CORE_ERROR,
                             MM_CORE_ERROR_FAILED,
                             "Unexpected group separator");
                g_ptr_array_unref (out);
                return NULL;
            }
            p++;
        }

        /* Skip whitespaces */
        while (*p == ' ' || *p == '\r' || *p == '\n')
            p++;

        /* New group */
        groups++;

        /* Empty group? */
        if (*p == ',' || *p == '\0') {
            g_ptr_array_add (out, g_strdup (""));
            continue;
        }

        /* No group start? */
        if (*p != '(') {
            /* Error */
            g_set_error (error,
                         MM_CORE_ERROR,
                         MM_CORE_ERROR_FAILED,
                         "Expected '(' not found");
            g_ptr_array_unref (out);
            return NULL;
        }
        p++;

        inner_groups = 0;
        start = p;
        while (TRUE) {
            if (*p == '(') {
                inner_groups++;
                p++;
                continue;
            }

            if (*p == '\0') {
                g_set_error (error,
                             MM_CORE_ERROR,
                             MM_CORE_ERROR_FAILED,
                             "Early end of string found, unfinished group");
                g_ptr_array_unref (out);
                return NULL;
            }

            if (*p == ')') {
                gchar *group;

                if (inner_groups > 0) {
                    inner_groups--;
                    p++;
                    continue;
                }

                group = g_strndup (start, p - start);
                g_ptr_array_add (out, group);
                p++;
                break;
            }

            /* keep on */
            p++;
        }
    }

    g_assert_not_reached ();
}

static gboolean
mode_from_syscfg (guint huawei_mode,
                  MMModemMode *modem_mode,
                  GError **error)
{
    g_assert (modem_mode != NULL);

    *modem_mode = MM_MODEM_MODE_NONE;
    switch (huawei_mode) {
    case 2:
        *modem_mode = MM_MODEM_MODE_2G | MM_MODEM_MODE_3G;
        break;
    case 13:
        *modem_mode = MM_MODEM_MODE_2G;
        break;
    case 14:
        *modem_mode = MM_MODEM_MODE_3G;
        break;
    case 16:
        /* ignore */
        break;
    default:
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "No translation from huawei prefmode '%u' to mode",
                     huawei_mode);
    }

    return *modem_mode != MM_MODEM_MODE_NONE ? TRUE : FALSE;
}

static GArray *
parse_syscfg_modes (const gchar *modes_str,
                    const gchar *acqorder_str,
                    GError **error)
{
    GArray *out;
    gchar **split;
    guint i;
    gint min_acqorder = 0;
    gint max_acqorder = 0;

    /* Start parsing acquisition order */
    if (!sscanf (acqorder_str, "%d-%d", &min_acqorder, &max_acqorder))
        mm_dbg ("Error parsing ^SYSCFG acquisition order range (%s)", acqorder_str);

    /* Just in case, we default to supporting only auto */
    if (max_acqorder < min_acqorder) {
        min_acqorder = 0;
        max_acqorder = 0;
    }

    /* Now parse modes */
    split = g_strsplit (modes_str, ",", -1);
    out = g_array_sized_new (FALSE,
                             FALSE,
                             sizeof (MMHuaweiSyscfgCombination),
                             g_strv_length (split));
    for (i = 0; split[i]; i++) {
        guint val;
        guint allowed = MM_MODEM_MODE_NONE;
        GError *inner_error = NULL;
        MMHuaweiSyscfgCombination combination;

        if (!mm_get_uint_from_str (mm_strip_quotes (split[i]), &val)) {
            mm_dbg ("Error parsing ^SYSCFG mode value: %s", split[i]);
            continue;
        }

        if (!mode_from_syscfg (val, &allowed, &inner_error)) {
            if (inner_error) {
                mm_dbg ("Unhandled ^SYSCFG: %s", inner_error->message);
                g_error_free (inner_error);
            }
            continue;
        }

        switch (allowed) {
        case MM_MODEM_MODE_2G:
        case MM_MODEM_MODE_3G:
            /* single mode */
            combination.allowed = allowed;
            combination.preferred = MM_MODEM_MODE_NONE;
            combination.mode = val;
            combination.acqorder = 0;
            g_array_append_val (out, combination);
            break;
        case (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G):
            /* 2G and 3G; auto */
            combination.allowed = allowed;
            combination.mode = val;
            if (min_acqorder == 0) {
                combination.preferred = MM_MODEM_MODE_NONE;
                combination.acqorder = 0;
                g_array_append_val (out, combination);
            }
            /* 2G and 3G; 2G preferred */
            if (min_acqorder <= 1 && max_acqorder >= 1) {
                combination.preferred = MM_MODEM_MODE_2G;
                combination.acqorder = 1;
                g_array_append_val (out, combination);
            }
            /* 2G and 3G; 3G preferred */
            if (min_acqorder <= 2 && max_acqorder >= 2) {
                combination.preferred = MM_MODEM_MODE_3G;
                combination.acqorder = 2;
                g_array_append_val (out, combination);
            }
            break;
        default:
            g_assert_not_reached ();
        }
    }

    g_strfreev (split);

    /* If we didn't build a valid array of combinations, return an error */
    if (out->len == 0) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Cannot parse list of allowed mode combinations: '%s,%s'",
                     modes_str,
                     acqorder_str);
        g_array_unref (out);
        return NULL;
    }

    return out;
}

GArray *
mm_huawei_parse_syscfg_test (const gchar *response,
                             GError **error)
{
    gchar **split;
    GError *inner_error = NULL;
    GArray *out;

    if (!response || !g_str_has_prefix (response, "^SYSCFG:")) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Missing ^SYSCFG prefix");
        return NULL;
    }

    /* Examples:
     *
     * ^SYSCFG:(2,13,14,16),
     *         (0-3),
     *         ((400000,"WCDMA2100")),
     *         (0-2),
     *         (0-4)
     */
    split = split_groups (mm_strip_tag (response, "^SYSCFG:"), error);
    if (!split)
        return NULL;

    /* We expect 5 string chunks */
    if (g_strv_length (split) < 5) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Unexpected ^SYSCFG format");
        g_strfreev (split);
        return FALSE;
    }

    /* Parse supported mode combinations */
    out = parse_syscfg_modes (split[0], split[1], &inner_error);

    g_strfreev (split);

    if (inner_error) {
        g_propagate_error (error, inner_error);
        return NULL;
    }

    return out;
}

/*****************************************************************************/
/* ^SYSCFG response parser */

const MMHuaweiSyscfgCombination *
mm_huawei_parse_syscfg_response (const gchar *response,
                                 const GArray *supported_mode_combinations,
                                 GError **error)
{
    gchar **split;
    guint mode;
    guint acqorder;
    guint i;

    if (!response || !g_str_has_prefix (response, "^SYSCFG:")) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Missing ^SYSCFG prefix");
        return NULL;
    }

    /* Format:
     *
     * ^SYSCFG: <mode>,<acqorder>,<band>,<roam>,<srvdomain>
     */

    response = mm_strip_tag (response, "^SYSCFG:");
    split = g_strsplit (response, ",", -1);

    /* We expect 5 string chunks */
    if (g_strv_length (split) < 5 ||
        !mm_get_uint_from_str (split[0], &mode) ||
        !mm_get_uint_from_str (split[1], &acqorder)) {
        /* Dump error to upper layer */
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Unexpected ^SYSCFG response: '%s'",
                     response);
        g_strfreev (split);
        return NULL;
    }

    /* Look for current modes among the supported ones */
    for (i = 0; i < supported_mode_combinations->len; i++) {
        const MMHuaweiSyscfgCombination *combination;

        combination = &g_array_index (supported_mode_combinations,
                                      MMHuaweiSyscfgCombination,
                                      i);
        if (mode == combination->mode && acqorder == combination->acqorder) {
            g_strfreev (split);
            return combination;
        }
    }

    g_set_error (error,
                 MM_CORE_ERROR,
                 MM_CORE_ERROR_FAILED,
                 "No SYSCFG combination found matching the current one (%d,%d)",
                 mode,
                 acqorder);
    g_strfreev (split);
    return NULL;
}

/*****************************************************************************/
/* ^SYSCFGEX test parser */

static void
huawei_syscfgex_combination_free (MMHuaweiSyscfgexCombination *item)
{
    /* Just the contents, not the item itself! */
    g_free (item->mode_str);
}

static gboolean
parse_mode_combination_string (const gchar *mode_str,
                               MMModemMode *allowed,
                               MMModemMode *preferred)
{
    guint n;

    if (g_str_equal (mode_str, "00")) {
        *allowed = MM_MODEM_MODE_ANY;
        *preferred = MM_MODEM_MODE_NONE;
        return TRUE;
    }

    *allowed = MM_MODEM_MODE_NONE;
    *preferred = MM_MODEM_MODE_NONE;

    for (n = 0; n < strlen (mode_str); n+=2) {
        MMModemMode mode;

        if (g_ascii_strncasecmp (&mode_str[n], "01", 2) == 0)
            /* GSM */
            mode = MM_MODEM_MODE_2G;
        else if (g_ascii_strncasecmp (&mode_str[n], "02", 2) == 0)
            /* WCDMA */
            mode = MM_MODEM_MODE_3G;
        else if (g_ascii_strncasecmp (&mode_str[n], "03", 2) == 0)
            /* LTE */
            mode = MM_MODEM_MODE_4G;
        else if (g_ascii_strncasecmp (&mode_str[n], "04", 2) == 0)
            /* CDMA Note: no EV-DO, just return single value, so assume CDMA1x*/
            mode = MM_MODEM_MODE_2G;
        else
            mode = MM_MODEM_MODE_NONE;

        if (mode != MM_MODEM_MODE_NONE) {
            /* The first one in the list is the preferred combination */
            if (n == 0)
                *preferred |= mode;
            *allowed |= mode;
        }
    }

    switch (mm_count_bits_set (*allowed)) {
    case 0:
        /* No allowed, error */
        return FALSE;
    case 1:
        /* If only one mode allowed, NONE preferred */
        *preferred = MM_MODEM_MODE_NONE;
        /* fall down */
    default:
        return TRUE;
    }
}

static GArray *
parse_mode_combination_string_list (const gchar *modes_str,
                                    GError **error)
{
    GArray *supported_mode_combinations;
    gchar **mode_combinations;
    MMModemMode all = MM_MODEM_MODE_NONE;
    gboolean has_all = FALSE;
    guint i;

    mode_combinations = g_strsplit (modes_str, ",", -1);
    supported_mode_combinations = g_array_sized_new (FALSE,
                                                     FALSE,
                                                     sizeof (MMHuaweiSyscfgexCombination),
                                                     g_strv_length (mode_combinations));
    g_array_set_clear_func (supported_mode_combinations,
                            (GDestroyNotify)huawei_syscfgex_combination_free);

    for (i = 0; mode_combinations[i]; i++) {
        MMHuaweiSyscfgexCombination combination;

        mode_combinations[i] = mm_strip_quotes (mode_combinations[i]);
        if (!parse_mode_combination_string (mode_combinations[i],
                                            &combination.allowed,
                                            &combination.preferred))
            continue;

        if (combination.allowed != MM_MODEM_MODE_ANY) {
            combination.mode_str = g_strdup (mode_combinations[i]);
            g_array_append_val (supported_mode_combinations, combination);

            all |= combination.allowed;
        } else {
            /* don't add the all_combination here, we may have more
             * combinations in the loop afterwards */
            has_all = TRUE;
        }
    }

    g_strfreev (mode_combinations);

    /* Add here the all_combination */
    if (has_all) {
        MMHuaweiSyscfgexCombination combination;

        combination.allowed = all;
        combination.preferred = MM_MODEM_MODE_NONE;
        combination.mode_str = g_strdup ("00");
        g_array_append_val (supported_mode_combinations, combination);
    }

    /* If we didn't build a valid array of combinations, return an error */
    if (supported_mode_combinations->len == 0) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Cannot parse list of allowed mode combinations: '%s'",
                     modes_str);
        g_array_unref (supported_mode_combinations);
        return NULL;
    }

    return supported_mode_combinations;
}

GArray *
mm_huawei_parse_syscfgex_test (const gchar *response,
                               GError **error)
{
    gchar **split;
    GError *inner_error = NULL;
    GArray *out;

    if (!response || !g_str_has_prefix (response, "^SYSCFGEX:")) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Missing ^SYSCFGEX prefix");
        return NULL;
    }

    /* Examples:
     *
     * ^SYSCFGEX: ("00","03","02","01","99"),
     *            ((2000004e80380,"GSM850/GSM900/GSM1800/GSM1900/WCDMA850/WCDMA900/WCDMA1900/WCDMA2100"),
     *             (3fffffff,"All Bands")),
     *            (0-3),
     *            (0-4),
     *            ((800c5,"LTE2100/LTE1800/LTE2600/LTE900/LTE800"),
     *             (7fffffffffffffff,"All bands"))
     */
    split = split_groups (mm_strip_tag (response, "^SYSCFGEX:"), error);
    if (!split)
        return NULL;

    /* We expect 5 string chunks */
    if (g_strv_length (split) < 5) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Unexpected ^SYSCFGEX format");
        g_strfreev (split);
        return NULL;
    }

    out = parse_mode_combination_string_list (split[0], &inner_error);

    g_strfreev (split);

    if (inner_error) {
        g_propagate_error (error, inner_error);
        return NULL;
    }

    return out;
}

/*****************************************************************************/
/* ^SYSCFGEX response parser */

const MMHuaweiSyscfgexCombination *
mm_huawei_parse_syscfgex_response (const gchar *response,
                                   const GArray *supported_mode_combinations,
                                   GError **error)
{
    gchar **split;
    guint i;
    gsize len;
    gchar *str;

    if (!response || !g_str_has_prefix (response, "^SYSCFGEX:")) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Missing ^SYSCFGEX prefix");
        return NULL;
    }

    /* Format:
     *
     * ^SYSCFGEX: "00",3FFFFFFF,1,2,7FFFFFFFFFFFFFFF
     * ^SYSCFGEX: <mode>,<band>,<roam>,<srvdomain>,<lte-band>
     */

    response = mm_strip_tag (response, "^SYSCFGEX:");
    split = g_strsplit (response, ",", -1);

    /* We expect 5 string chunks */
    if (g_strv_length (split) < 5) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Unexpected ^SYSCFGEX response format");
        g_strfreev (split);
        return NULL;
    }

    /* Unquote */
    str = split[0];
    len = strlen (str);
    if ((len >= 2) && (str[0] == '"') && (str[len - 1] == '"')) {
        str[0] = ' ';
        str[len - 1] = ' ';
        str = g_strstrip (str);
    }

    /* Look for current modes among the supported ones */
    for (i = 0; i < supported_mode_combinations->len; i++) {
        const MMHuaweiSyscfgexCombination *combination;

        combination = &g_array_index (supported_mode_combinations,
                                      MMHuaweiSyscfgexCombination,
                                      i);
        if (g_str_equal (str, combination->mode_str)) {
            g_strfreev (split);
            return combination;
        }
    }

    g_set_error (error,
                 MM_CORE_ERROR,
                 MM_CORE_ERROR_FAILED,
                 "No SYSCFGEX combination found matching the current one (%s)",
                 str);
    g_strfreev (split);
    return NULL;
}

/*****************************************************************************/
/* ^NWTIME response parser */

gboolean mm_huawei_parse_nwtime_response (const gchar *response,
                                          gchar **iso8601p,
                                          MMNetworkTimezone **tzp,
                                          GError **error)
{
    GRegex *r;
    GMatchInfo *match_info = NULL;
    GError *match_error = NULL;
    guint year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, dt = 0;
    gint tz = 0;
    gboolean ret = FALSE;

    g_assert (iso8601p || tzp); /* at least one */

    r = g_regex_new ("\\^NWTIME:\\s*(\\d+)/(\\d+)/(\\d+),(\\d+):(\\d+):(\\d*)([\\-\\+\\d]+),(\\d+)$", 0, 0, NULL);
    g_assert (r != NULL);

    if (!g_regex_match_full (r, response, -1, 0, 0, &match_info, &match_error)) {
        if (match_error) {
            g_propagate_error (error, match_error);
            g_prefix_error (error, "Could not parse ^NWTIME results: ");
        } else {
            g_set_error_literal (error,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_FAILED,
                                 "Couldn't match ^NWTIME reply");
        }
    } else {
        /* Remember that g_match_info_get_match_count() includes match #0 */
        g_assert (g_match_info_get_match_count (match_info) >= 9);

        if (mm_get_uint_from_match_info (match_info, 1, &year) &&
            mm_get_uint_from_match_info (match_info, 2, &month) &&
            mm_get_uint_from_match_info (match_info, 3, &day) &&
            mm_get_uint_from_match_info (match_info, 4, &hour) &&
            mm_get_uint_from_match_info (match_info, 5, &minute) &&
            mm_get_uint_from_match_info (match_info, 6, &second) &&
            mm_get_int_from_match_info  (match_info, 7, &tz) &&
            mm_get_uint_from_match_info (match_info, 8, &dt)) {
            /* adjust year */
            if (year < 100)
                year += 2000;
            /*
             * tz = timezone offset in 15 minute intervals
             * dt = daylight adjustment, 0 = none, 1 = 1 hour, 2 = 2 hours
             *      other values are marked reserved.
             */
            if (iso8601p) {
                /* Return ISO-8601 format date/time string */
                *iso8601p = mm_new_iso8601_time (year, month, day, hour,
                                                 minute, second,
                                                 TRUE, (tz * 15) + (dt * 60));
            }
            if (tzp) {
                *tzp = mm_network_timezone_new ();
                mm_network_timezone_set_offset (*tzp, tz * 15);
                mm_network_timezone_set_dst_offset (*tzp, dt * 60);
            }

            ret = TRUE;
        } else {
            g_set_error_literal (error,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_FAILED,
                                 "Failed to parse ^NWTIME reply");
        }
    }

    if (match_info)
        g_match_info_free (match_info);
    g_regex_unref (r);

    return ret;
}

/*****************************************************************************/
/* ^TIME response parser */

gboolean mm_huawei_parse_time_response (const gchar *response,
                                        gchar **iso8601p,
                                        MMNetworkTimezone **tzp,
                                        GError **error)
{
    GRegex *r;
    GMatchInfo *match_info = NULL;
    GError *match_error = NULL;
    guint year, month, day, hour, minute, second;
    gboolean ret = FALSE;

    g_assert (iso8601p || tzp); /* at least one */

    /* TIME response cannot ever provide TZ info */
    if (tzp) {
        g_set_error_literal (error,
                             MM_CORE_ERROR,
                             MM_CORE_ERROR_UNSUPPORTED,
                             "^TIME does not provide timezone information");
        return FALSE;
    }

    /* Already in ISO-8601 format, but verify just to be sure */
    r = g_regex_new ("\\^TIME:\\s*(\\d+)/(\\d+)/(\\d+)\\s*(\\d+):(\\d+):(\\d*)$", 0, 0, NULL);
    g_assert (r != NULL);

    if (!g_regex_match_full (r, response, -1, 0, 0, &match_info, &match_error)) {
        if (match_error) {
            g_propagate_error (error, match_error);
            g_prefix_error (error, "Could not parse ^TIME results: ");
        } else {
            g_set_error_literal (error,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_FAILED,
                                 "Couldn't match ^TIME reply");
        }
    } else {
        /* Remember that g_match_info_get_match_count() includes match #0 */
        g_assert (g_match_info_get_match_count (match_info) >= 7);

        if (mm_get_uint_from_match_info (match_info, 1, &year) &&
            mm_get_uint_from_match_info (match_info, 2, &month) &&
            mm_get_uint_from_match_info (match_info, 3, &day) &&
            mm_get_uint_from_match_info (match_info, 4, &hour) &&
            mm_get_uint_from_match_info (match_info, 5, &minute) &&
            mm_get_uint_from_match_info (match_info, 6, &second)) {
            /* adjust year */
            if (year < 100)
                year += 2000;
            /* Return ISO-8601 format date/time string */
            if (iso8601p)
                *iso8601p = mm_new_iso8601_time (year, month, day, hour,
                                                 minute, second, FALSE, 0);
            ret = TRUE;
        } else {
            g_set_error_literal (error,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_FAILED,
                                 "Failed to parse ^TIME reply");
        }
    }

    if (match_info)
        g_match_info_free (match_info);
    g_regex_unref (r);

    return ret;
}

