/*
 * Copyright (C) 2012-2013 Red Hat, Inc.
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

#ifndef TESTSYS_H
#define TESTSYS_H


#include <solv/pooltypes.h>


#include "libhif/hif-sack.h"
#include "testshared.h"

void assert_nevra_eq(HifPackage *pkg, const char *nevra);
HifPackage *by_name(HifSack *sack, const char *name);
HifPackage *by_name_repo(HifSack *sack, const char *name, const char *repo);
void dump_packagelist(GPtrArray *plist, int free);
void dump_query_results(HyQuery query);
void dump_goal_results(HyGoal goal);
int query_count_results(HyQuery query);

#endif /* TESTSYS_H */
