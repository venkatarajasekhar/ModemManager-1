/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * Copyright (C) 2011 Red Hat, Inc.
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "errors.h"
#include <stdlib.h>
#include <string.h>

void
_wmc_log (const char *file,
          int line,
          const char *func,
          int level,
          int domain,
          const char *format,
          ...)
{
    va_list args;
    char *message = NULL;
    int n;
    const char *prefix = "info";

    wmc_return_if_fail (format != NULL);
    wmc_return_if_fail (format[0] != '\0');

    /* level & domain ignored for now */

    if (getenv ("WMC_DEBUG") == NULL)
        return;

    va_start (args, format);
    n = vasprintf (&message, format, args);
    va_end (args);

    if (level & LOGL_ERR)
        prefix = "err";
    else if (level & LOGL_DEBUG)
        prefix = "dbg";

    if (n >= 0) {
        fprintf (stderr, "<%s> [%s:%u] %s(): %s\n", prefix, file, line, func, message);
        free (message);
    }
}

