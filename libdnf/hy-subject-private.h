/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "hy-types.h"

#ifndef HY_SUBJECT_INTERNAL_H
#define HY_SUBJECT_INTERNAL_H

enum poss_type {
    TYPE_NEVRA,
    TYPE_RELDEP_NEW,
    TYPE_RELDEP_END
};

extern const char *nevra_form_regex[];

int nevra_possibility(char *nevra_str, int re, HyNevra nevra);

#endif
