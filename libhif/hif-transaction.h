/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * Copyright (C) 2014-2015 Richard Hughes <richard@hughsie.com>
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef __HIF_TRANSACTION_H
#define __HIF_TRANSACTION_H

#include <glib-object.h>
#include <gio/gio.h>

#include "hy-goal.h"

#include "hif-context.h"
#include "hif-db.h"
#include "hif-state.h"
#include "hif-repos.h"

G_BEGIN_DECLS

#define HIF_TYPE_TRANSACTION (hif_transaction_get_type ())
G_DECLARE_DERIVABLE_TYPE (HifTransaction, hif_transaction, HIF, TRANSACTION, GObject)

struct _HifTransactionClass
{
        GObjectClass            parent_class;
        /*< private >*/
        void (*_hif_reserved1)  (void);
        void (*_hif_reserved2)  (void);
        void (*_hif_reserved3)  (void);
        void (*_hif_reserved4)  (void);
        void (*_hif_reserved5)  (void);
        void (*_hif_reserved6)  (void);
        void (*_hif_reserved7)  (void);
        void (*_hif_reserved8)  (void);
};

/**
 * HifTransactionFlag:
 * @HIF_TRANSACTION_FLAG_NONE:                  No flags
 * @HIF_TRANSACTION_FLAG_ONLY_TRUSTED:          Only install trusted packages
 * @HIF_TRANSACTION_FLAG_ALLOW_REINSTALL:       Allow package reinstallation
 * @HIF_TRANSACTION_FLAG_ALLOW_DOWNGRADE:       Allow package downrades
 * @HIF_TRANSACTION_FLAG_NODOCS:                Don't install documentation
 * @HIF_TRANSACTION_FLAG_TEST:                  Only do a transaction test
 *
 * The transaction flags.
 **/
typedef enum {
        HIF_TRANSACTION_FLAG_NONE               = 0,
        HIF_TRANSACTION_FLAG_ONLY_TRUSTED       = 1 << 0,
        HIF_TRANSACTION_FLAG_ALLOW_REINSTALL    = 1 << 1,
        HIF_TRANSACTION_FLAG_ALLOW_DOWNGRADE    = 1 << 2,
        HIF_TRANSACTION_FLAG_NODOCS             = 1 << 3,
        HIF_TRANSACTION_FLAG_TEST               = 1 << 4,
        /*< private >*/
        HIF_TRANSACTION_FLAG_LAST
} HifTransactionFlag;

HifTransaction  *hif_transaction_new                    (HifContext     *context);

/* getters */
guint64          hif_transaction_get_flags              (HifTransaction *transaction);
GPtrArray       *hif_transaction_get_remote_pkgs        (HifTransaction *transaction);
HifDb           *hif_transaction_get_db                 (HifTransaction *transaction);

/* setters */
void             hif_transaction_set_repos            (HifTransaction *transaction,
                                                         GPtrArray      *repos);
void             hif_transaction_set_uid                (HifTransaction *transaction,
                                                         guint           uid);
void             hif_transaction_set_flags              (HifTransaction *transaction,
                                                         guint64         flags);

/* object methods */
gboolean         hif_transaction_depsolve               (HifTransaction *transaction,
                                                         HyGoal          goal,
                                                         HifState       *state,
                                                         GError         **error);
gboolean         hif_transaction_download               (HifTransaction *transaction,
                                                         HifState       *state,
                                                         GError         **error);
gboolean         hif_transaction_commit                 (HifTransaction *transaction,
                                                         HyGoal          goal,
                                                         HifState       *state,
                                                         GError         **error);
gboolean         hif_transaction_ensure_repo          (HifTransaction *transaction,
                                                         HifPackage *      pkg,
                                                         GError         **error);
gboolean         hif_transaction_ensure_repo_list     (HifTransaction *transaction,
                                                         GPtrArray *  pkglist,
                                                         GError         **error);

G_END_DECLS

#endif /* __HIF_TRANSACTION_H */
