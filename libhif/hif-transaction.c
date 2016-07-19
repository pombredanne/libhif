/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * Copyright (C) 2014-2015 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or(at your option) any later version.
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

/**
 * SECTION:hif-transaction
 * @short_description: High level interface to librpm.
 * @include: libhif.h
 * @stability: Stable
 *
 * This object represents an RPM transaction.
 */


#include <rpm/rpmlib.h>
#include <rpm/rpmts.h>
#include <rpm/rpmlog.h>

#include "hif-db.h"
#include "hif-goal.h"
#include "hif-keyring.h"
#include "hif-package.h"
#include "hif-rpmts.h"
#include "hif-transaction.h"
#include "hif-utils.h"
#include "hy-query.h"
#include "hif-sack.h"
#include "hy-util.h"

typedef enum {
    HIF_TRANSACTION_STEP_STARTED,
    HIF_TRANSACTION_STEP_PREPARING,
    HIF_TRANSACTION_STEP_WRITING,
    HIF_TRANSACTION_STEP_IGNORE
} HifTransactionStep;

typedef struct
{
    HifDb              *db;
    rpmKeyring          keyring;
    rpmts               ts;
    HifContext         *context;    /* weak reference */
    GPtrArray          *repos;
    guint               uid;

    /* previously in the helper */
    HifState            *state;
    HifState            *child;
    FD_t                 fd;
    HifTransactionStep   step;
    GTimer              *timer;
    guint                last_progress;
    GPtrArray           *remove;
    GPtrArray           *remove_helper;
    GPtrArray           *install;
    GPtrArray           *pkgs_to_download;
    GHashTable          *erased_by_package_hash;
    guint64             flags;
} HifTransactionPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(HifTransaction, hif_transaction, G_TYPE_OBJECT)
#define GET_PRIVATE(o) (hif_transaction_get_instance_private (o))

/**
 * hif_transaction_finalize:
 **/
static void
hif_transaction_finalize(GObject *object)
{
    HifTransaction *transaction = HIF_TRANSACTION(object);
    HifTransactionPrivate *priv = GET_PRIVATE(transaction);

    g_ptr_array_unref(priv->pkgs_to_download);
    g_timer_destroy(priv->timer);
    rpmKeyringFree(priv->keyring);
    rpmtsFree(priv->ts);

    if (priv->db != NULL)
        g_object_unref(priv->db);
    if (priv->repos != NULL)
        g_ptr_array_unref(priv->repos);
    if (priv->install != NULL)
        g_ptr_array_unref(priv->install);
    if (priv->remove != NULL)
        g_ptr_array_unref(priv->remove);
    if (priv->remove_helper != NULL)
        g_ptr_array_unref(priv->remove_helper);
    if (priv->erased_by_package_hash != NULL)
        g_hash_table_unref(priv->erased_by_package_hash);
    if (priv->context != NULL)
        g_object_remove_weak_pointer(G_OBJECT(priv->context),
                                     (void **) &priv->context);

    G_OBJECT_CLASS(hif_transaction_parent_class)->finalize(object);
}

/**
 * hif_transaction_init:
 **/
static void
hif_transaction_init(HifTransaction *transaction)
{
    HifTransactionPrivate *priv = GET_PRIVATE(transaction);
    priv->ts = rpmtsCreate();
    priv->keyring = rpmtsGetKeyring(priv->ts, 1);
    priv->timer = g_timer_new();
    priv->pkgs_to_download = g_ptr_array_new_with_free_func((GDestroyNotify) g_object_unref);
}

/**
 * hif_transaction_class_init:
 **/
static void
hif_transaction_class_init(HifTransactionClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = hif_transaction_finalize;
}

/**
 * hif_transaction_get_flags:
 * @transaction: a #HifTransaction instance.
 *
 * Gets the transaction flags.
 *
 * Returns: the transaction flags used for this transaction
 *
 * Since: 0.1.0
 **/
guint64
hif_transaction_get_flags(HifTransaction *transaction)
{
    HifTransactionPrivate *priv = GET_PRIVATE(transaction);
    return priv->flags;
}

/**
 * hif_transaction_get_remote_pkgs:
 * @transaction: a #HifTransaction instance.
 *
 * Gets the packages that will be downloaded in hif_transaction_download().
 *
 * The hif_transaction_depsolve() function must have been called before this
 * function will return sensible results.
 *
 * Returns:(transfer none): the list of packages
 *
 * Since: 0.1.0
 **/
GPtrArray *
hif_transaction_get_remote_pkgs(HifTransaction *transaction)
{
    HifTransactionPrivate *priv = GET_PRIVATE(transaction);
    return priv->pkgs_to_download;
}

/**
 * hif_transaction_get_db:
 * @transaction: a #HifTransaction instance.
 *
 * Gets the database instance used for this transaction.
 *
 * Returns:(transfer none): the #HifDb
 *
 * Since: 0.1.0
 **/
HifDb *
hif_transaction_get_db(HifTransaction *transaction)
{
    HifTransactionPrivate *priv = GET_PRIVATE(transaction);
    return priv->db;
}

/**
 * hif_transaction_set_repos:
 * @transaction: a #HifTransaction instance.
 * @repos: the repos to use with the transaction
 *
 * Sets the list of repos.
 *
 * Since: 0.1.0
 **/
void
hif_transaction_set_repos(HifTransaction *transaction, GPtrArray *repos)
{
    HifTransactionPrivate *priv = GET_PRIVATE(transaction);
    if (priv->repos != NULL)
        g_ptr_array_unref(priv->repos);
    priv->repos = g_ptr_array_ref(repos);
}

/**
 * hif_transaction_set_uid:
 * @transaction: a #HifTransaction instance.
 * @uid: the uid
 *
 * Sets the user ID for the person who started this transaction.
 *
 * Since: 0.1.0
 **/
void
hif_transaction_set_uid(HifTransaction *transaction, guint uid)
{
    HifTransactionPrivate *priv = GET_PRIVATE(transaction);
    priv->uid = uid;
}

/**
 * hif_transaction_set_flags:
 * @transaction: a #HifTransaction instance.
 * @flags: the flags, e.g. %HIF_TRANSACTION_FLAG_ONLY_TRUSTED
 *
 * Sets the flags used for this transaction.
 *
 * Since: 0.1.0
 **/
void
hif_transaction_set_flags(HifTransaction *transaction, guint64 flags)
{
    HifTransactionPrivate *priv = GET_PRIVATE(transaction);
    priv->flags = flags;
}

/**
 * hif_transaction_ensure_repo:
 * @transaction: a #HifTransaction instance.
 * @pkg: A #HifPackage
 * @error: A #GError or %NULL
 *
 * Ensures the #HifRepo is set on the #HifPackage *if not already set.
 *
 * Returns: %TRUE for success, %FALSE otherwise
 *
 * Since: 0.1.0
 */
gboolean
hif_transaction_ensure_repo(HifTransaction *transaction,
                              HifPackage *pkg,
                              GError **error)
{
    HifRepo *repo;
    HifTransactionPrivate *priv = GET_PRIVATE(transaction);
    char *location;
    guint i;

    /* not set yet */
    if (priv->repos == NULL) {
        g_set_error(error,
                    HIF_ERROR,
                    HIF_ERROR_INTERNAL_ERROR,
                    "Sources not set when trying to ensure package %s",
                    hif_package_get_name(pkg));
        return FALSE;
    }

    /* this is a local file */
    if (g_strcmp0(hif_package_get_reponame(pkg),
                  HY_CMDLINE_REPO_NAME) == 0) {
        location = hif_package_get_location(pkg);
        hif_package_set_filename(pkg, location);
        g_free(location);
        return TRUE;
    }

    /* get repo */
    if (hif_package_installed(pkg))
        return TRUE;
    for (i = 0; i < priv->repos->len; i++) {
        repo = g_ptr_array_index(priv->repos, i);
        if (g_strcmp0(hif_package_get_reponame(pkg),
                      hif_repo_get_id(repo)) == 0) {
            hif_package_set_repo(pkg, repo);
            return TRUE;
        }
    }

    /* not found */
    g_set_error(error,
                HIF_ERROR,
                HIF_ERROR_INTERNAL_ERROR,
                "Failed to ensure %s as repo %s not "
                "found(%i repos loaded)",
                hif_package_get_name(pkg),
                hif_package_get_reponame(pkg),
                priv->repos->len);
    return FALSE;
}

/**
 * hif_transaction_ensure_repo_list:
 * @transaction: a #HifTransaction instance.
 * @pkglist: A #GPtrArray *
 * @error: A #GError or %NULL
 *
 * Ensures the #HifRepo is set on the #GPtrArray *if not already set.
 *
 * Returns: %TRUE for success, %FALSE otherwise
 *
 * Since: 0.1.0
 */
gboolean
hif_transaction_ensure_repo_list(HifTransaction *transaction,
                                   GPtrArray *pkglist,
                                   GError **error)
{
    guint i;
    HifPackage *pkg;

    for (i = 0; i < pkglist->len; i++) {
        pkg = g_ptr_array_index (pkglist, i);
        if (!hif_transaction_ensure_repo(transaction, pkg, error))
            return FALSE;
    }
    return TRUE;
}

/**
 * hif_transaction_check_untrusted:
 */
static gboolean
hif_transaction_check_untrusted(HifTransaction *transaction,
                                HyGoal goal,
                                GError **error)
{
    HifTransactionPrivate *priv = GET_PRIVATE(transaction);
    HifPackage *pkg;
    const gchar *fn;
    guint i;
    g_autoptr(GPtrArray) install = NULL;

    /* find a list of all the packages we might have to download */
    install = hif_goal_get_packages(goal,
                                    HIF_PACKAGE_INFO_INSTALL,
                                    HIF_PACKAGE_INFO_REINSTALL,
                                    HIF_PACKAGE_INFO_DOWNGRADE,
                                    HIF_PACKAGE_INFO_UPDATE,
                                    -1);
    if (install->len == 0)
        return TRUE;

    /* find any packages in untrusted repos */
    for (i = 0; i < install->len; i++) {
        GError *error_local = NULL;
        HifRepo *repo;
        pkg = g_ptr_array_index(install, i);

        /* ensure the filename is set */
        if (!hif_transaction_ensure_repo(transaction, pkg, error)) {
            g_prefix_error(error, "Failed to check untrusted: ");
            return FALSE;
        }

        /* find the location of the local file */
        fn = hif_package_get_filename(pkg);
        if (fn == NULL) {
            g_set_error(error,
                        HIF_ERROR,
                        HIF_ERROR_FILE_NOT_FOUND,
                        "Downloaded file for %s not found",
                        hif_package_get_name(pkg));
            return FALSE;
        }

        /* check file */
        if (!hif_keyring_check_untrusted_file(priv->keyring, fn, &error_local)) {

            /* probably an i/o error */
            if (!g_error_matches(error_local,
                                 HIF_ERROR,
                                 HIF_ERROR_GPG_SIGNATURE_INVALID)) {
                g_propagate_error(error, error_local);
                return FALSE;
            }

            /* if the repo is signed this is ALWAYS an error */
            repo = hif_package_get_repo(pkg);
            if (repo != NULL && hif_repo_get_gpgcheck(repo)) {
                g_set_error(error,
                            HIF_ERROR,
                            HIF_ERROR_FILE_INVALID,
                            "package %s cannot be verified "
                            "and repo %s is GPG enabled: %s",
                            hif_package_get_nevra(pkg),
                            hif_repo_get_id(repo),
                            error_local->message);
                g_error_free(error_local);
                return FALSE;
            }

            /* we can only install signed packages in this mode */
            if ((priv->flags & HIF_TRANSACTION_FLAG_ONLY_TRUSTED) > 0) {
                g_propagate_error(error, error_local);
                return FALSE;
            }

            /* we can install unsigned packages */
            g_debug("ignoring as allow-untrusted: %s",
                 error_local->message);
            g_clear_error(&error_local);
        }
    }
    return TRUE;
}

/**
 * hif_transaction_rpmcb_type_to_string:
 **/
static const gchar *
hif_transaction_rpmcb_type_to_string(const rpmCallbackType what)
{
    const gchar *type = NULL;
    switch(what) {
    case RPMCALLBACK_UNKNOWN:
        type = "unknown";
        break;
    case RPMCALLBACK_INST_PROGRESS:
        type = "install-progress";
        break;
    case RPMCALLBACK_INST_START:
        type = "install-start";
        break;
    case RPMCALLBACK_INST_OPEN_FILE:
        type = "install-open-file";
        break;
    case RPMCALLBACK_INST_CLOSE_FILE:
        type = "install-close-file";
        break;
    case RPMCALLBACK_TRANS_PROGRESS:
        type = "transaction-progress";
        break;
    case RPMCALLBACK_TRANS_START:
        type = "transaction-start";
        break;
    case RPMCALLBACK_TRANS_STOP:
        type = "transaction-stop";
        break;
    case RPMCALLBACK_UNINST_PROGRESS:
        type = "uninstall-progress";
        break;
    case RPMCALLBACK_UNINST_START:
        type = "uninstall-start";
        break;
    case RPMCALLBACK_UNINST_STOP:
        type = "uninstall-stop";
        break;
    case RPMCALLBACK_REPACKAGE_PROGRESS:
        type = "repackage-progress";
        break;
    case RPMCALLBACK_REPACKAGE_START:
        type = "repackage-start";
        break;
    case RPMCALLBACK_REPACKAGE_STOP:
        type = "repackage-stop";
        break;
    case RPMCALLBACK_UNPACK_ERROR:
        type = "unpack-error";
        break;
    case RPMCALLBACK_CPIO_ERROR:
        type = "cpio-error";
        break;
    case RPMCALLBACK_SCRIPT_ERROR:
        type = "script-error";
        break;
    case RPMCALLBACK_SCRIPT_START:
        type = "script-start";
        break;
    case RPMCALLBACK_SCRIPT_STOP:
        type = "script-stop";
        break;
    case RPMCALLBACK_INST_STOP:
        type = "install-stop";
        break;
    }
    return type;
}

/**
 * hif_find_pkg_from_header:
 **/
static HifPackage *
hif_find_pkg_from_header(GPtrArray *array, Header hdr)
{
    const gchar *arch;
    const gchar *name;
    const gchar *release;
    const gchar *version;
    guint epoch;
    guint i;
    HifPackage *pkg;

    /* get details */
    name = headerGetString(hdr, RPMTAG_NAME);
    epoch = headerGetNumber(hdr, RPMTAG_EPOCH);
    version = headerGetString(hdr, RPMTAG_VERSION);
    release = headerGetString(hdr, RPMTAG_RELEASE);
    arch = headerGetString(hdr, RPMTAG_ARCH);

    /* find in array */
    for (i = 0; i < array->len; i++) {
        pkg = g_ptr_array_index(array, i);
        if (g_strcmp0(name, hif_package_get_name(pkg)) != 0)
            continue;
        if (g_strcmp0(version, hif_package_get_version(pkg)) != 0)
            continue;
        if (g_strcmp0(release, hif_package_get_release(pkg)) != 0)
            continue;
        if (g_strcmp0(arch, hif_package_get_arch(pkg)) != 0)
            continue;
        if (epoch != hif_package_get_epoch(pkg))
            continue;
        return pkg;
    }
    return NULL;
}

/**
 * hif_find_pkg_from_filename_suffix:
 **/
static HifPackage *
hif_find_pkg_from_filename_suffix(GPtrArray *array, const gchar *filename_suffix)
{
    const gchar *filename;
    guint i;
    HifPackage *pkg;

    /* find in array */
    for (i = 0; i < array->len; i++) {
        pkg = g_ptr_array_index(array, i);
        filename = hif_package_get_filename(pkg);
        if (filename == NULL)
            continue;
        if (g_str_has_suffix(filename, filename_suffix))
            return pkg;
    }
    return NULL;
}

/**
 * hif_find_pkg_from_name:
 **/
static HifPackage *
hif_find_pkg_from_name(GPtrArray *array, const gchar *pkgname)
{
    guint i;
    HifPackage *pkg;

    /* find in array */
    for (i = 0; i < array->len; i++) {
        pkg = g_ptr_array_index(array, i);
        if (g_strcmp0(hif_package_get_name(pkg), pkgname) == 0)
            return pkg;
    }
    return NULL;
}

/**
 * hif_transaction_ts_progress_cb:
 **/
static void *
hif_transaction_ts_progress_cb(const void *arg,
                               const rpmCallbackType what,
                               const rpm_loff_t amount,
                               const rpm_loff_t total,
                               fnpyKey key, void *data)
{
    HifTransaction *transaction = HIF_TRANSACTION(data);
    HifTransactionPrivate *priv = GET_PRIVATE(transaction);
    const char *filename =(const char *) key;
    const gchar *name = NULL;
    gboolean ret;
    guint percentage;
    guint speed;
    Header hdr =(Header) arg;
    HifPackage *pkg;
    HifStateAction action;
    void *rc = NULL;
    g_autoptr(GError) error_local = NULL;

    if (hdr != NULL)
        name = headerGetString(hdr, RPMTAG_NAME);
    g_debug("phase: %s(%i/%i, %s/%s)",
            hif_transaction_rpmcb_type_to_string(what),
           (gint32) amount,
           (gint32) total,
           (const gchar *) key,
            name);

    switch(what) {
    case RPMCALLBACK_INST_OPEN_FILE:

        /* valid? */
        if (filename == NULL || filename[0] == '\0')
            return NULL;

        /* open the file and return file descriptor */
        priv->fd = Fopen(filename, "r.ufdio");
        return(void *) priv->fd;
        break;

    case RPMCALLBACK_INST_CLOSE_FILE:

        /* just close the file */
        if (priv->fd != NULL) {
            Fclose(priv->fd);
            priv->fd = NULL;
        }
        break;

    case RPMCALLBACK_INST_START:

        /* find pkg */
        pkg = hif_find_pkg_from_filename_suffix(priv->install,
                                                filename);
        if (pkg == NULL)
            g_assert_not_reached();

        /* map to correct action code */
        action = hif_package_get_action(pkg);
        if (action == HIF_STATE_ACTION_UNKNOWN)
            action = HIF_STATE_ACTION_INSTALL;

        /* set the pkgid if not already set */
        if (hif_package_get_pkgid(pkg) == NULL) {
            const gchar *pkgid;
            pkgid = headerGetString(hdr, RPMTAG_SHA1HEADER);
            if (pkgid != NULL) {
                g_debug("setting %s pkgid %s", name, pkgid);
                hif_package_set_pkgid(pkg, pkgid);
            }
        }

        /* install start */
        priv->step = HIF_TRANSACTION_STEP_WRITING;
        priv->child = hif_state_get_child(priv->state);
        hif_state_action_start(priv->child,
                               action,
                               hif_package_get_package_id(pkg));
        g_debug("install start: %s size=%i", filename,(gint32) total);
        break;

    case RPMCALLBACK_UNINST_START:

        /* find pkg */
        pkg = hif_find_pkg_from_header(priv->remove, hdr);
        if (pkg == NULL && filename != NULL) {
            pkg = hif_find_pkg_from_filename_suffix(priv->remove,
                                                    filename);
        }
        if (pkg == NULL && name != NULL)
            pkg = hif_find_pkg_from_name(priv->remove, name);
        if (pkg == NULL && name != NULL)
            pkg = hif_find_pkg_from_name(priv->remove_helper, name);
        if (pkg == NULL) {
            g_warning("cannot find %s in uninst-start", name);
            priv->step = HIF_TRANSACTION_STEP_WRITING;
            break;
        }

        /* map to correct action code */
        action = hif_package_get_action(pkg);
        if (action == HIF_STATE_ACTION_UNKNOWN)
            action = HIF_STATE_ACTION_REMOVE;

        /* remove start */
        priv->step = HIF_TRANSACTION_STEP_WRITING;
        priv->child = hif_state_get_child(priv->state);
        hif_state_action_start(priv->child,
                               action,
                               hif_package_get_package_id(pkg));
        g_debug("remove start: %s size=%i", filename,(gint32) total);
        break;

    case RPMCALLBACK_TRANS_PROGRESS:
    case RPMCALLBACK_INST_PROGRESS:

        /* we're preparing the transaction */
        if (priv->step == HIF_TRANSACTION_STEP_PREPARING ||
            priv->step == HIF_TRANSACTION_STEP_IGNORE) {
            g_debug("ignoring preparing %i / %i",
                    (gint32) amount,(gint32) total);
            break;
        }

        /* work out speed */
        speed = (amount - priv->last_progress) /
                    g_timer_elapsed(priv->timer, NULL);
        hif_state_set_speed(priv->state, speed);
        priv->last_progress = amount;
        g_timer_reset(priv->timer);

        /* progress */
        percentage =(100.0f /(gfloat) total) *(gfloat) amount;
        if (priv->child != NULL)
            hif_state_set_percentage(priv->child, percentage);

        /* update UI */
        pkg = hif_find_pkg_from_header(priv->install, hdr);
        if (pkg == NULL) {
            pkg = hif_find_pkg_from_filename_suffix(priv->install,
                                                    filename);
        }
        if (pkg == NULL) {
            g_debug("cannot find %s(%s)", filename, name);
            break;
        }

        hif_state_set_package_progress(priv->state,
                                       hif_package_get_package_id(pkg),
                                       HIF_STATE_ACTION_INSTALL,
                                       percentage);
        break;

    case RPMCALLBACK_UNINST_PROGRESS:

        /* we're preparing the transaction */
        if (priv->step == HIF_TRANSACTION_STEP_PREPARING ||
            priv->step == HIF_TRANSACTION_STEP_IGNORE) {
            g_debug("ignoring preparing %i / %i",
                    (gint32) amount,(gint32) total);
            break;
        }

        /* progress */
        percentage =(100.0f /(gfloat) total) *(gfloat) amount;
        if (priv->child != NULL)
            hif_state_set_percentage(priv->child, percentage);

        /* update UI */
        pkg = hif_find_pkg_from_header(priv->remove, hdr);
        if (pkg == NULL && filename != NULL) {
            pkg = hif_find_pkg_from_filename_suffix(priv->remove,
                                                    filename);
        }
        if (pkg == NULL && name != NULL)
            pkg = hif_find_pkg_from_name(priv->remove, name);
        if (pkg == NULL && name != NULL)
            pkg = hif_find_pkg_from_name(priv->remove_helper, name);
        if (pkg == NULL) {
            g_warning("cannot find %s in uninst-progress", name);
            break;
        }

        /* map to correct action code */
        action = hif_package_get_action(pkg);
        if (action == HIF_STATE_ACTION_UNKNOWN)
            action = HIF_STATE_ACTION_REMOVE;

        hif_state_set_package_progress(priv->state,
                                       hif_package_get_package_id(pkg),
                                       action,
                                       percentage);
        break;

    case RPMCALLBACK_TRANS_START:

        /* we setup the state */
        g_debug("preparing transaction with %i items",(gint32) total);
        if (priv->step == HIF_TRANSACTION_STEP_IGNORE)
            break;

        hif_state_set_number_steps(priv->state, total);
        priv->step = HIF_TRANSACTION_STEP_PREPARING;
        break;

    case RPMCALLBACK_TRANS_STOP:

        /* don't do anything */
        break;

    case RPMCALLBACK_INST_STOP:
    case RPMCALLBACK_UNINST_STOP:

        /* phase complete */
        ret = hif_state_done(priv->state, &error_local);
        if (!ret) {
            g_warning("state increment failed: %s",
                      error_local->message);
        }
        break;

    case RPMCALLBACK_UNPACK_ERROR:
    case RPMCALLBACK_CPIO_ERROR:
    case RPMCALLBACK_SCRIPT_ERROR:
    case RPMCALLBACK_SCRIPT_START:
    case RPMCALLBACK_SCRIPT_STOP:
    case RPMCALLBACK_UNKNOWN:
    case RPMCALLBACK_REPACKAGE_PROGRESS:
    case RPMCALLBACK_REPACKAGE_START:
    case RPMCALLBACK_REPACKAGE_STOP:
        g_debug("%s uninteresting",
                hif_transaction_rpmcb_type_to_string(what));
        break;
    default:
        g_warning("unknown transaction phase: %u(%s)",
                  what,
                  hif_transaction_rpmcb_type_to_string(what));
        break;
    }
    return rc;
}

/**
 * hif_rpm_verbosity_string_to_value:
 **/
static gint
hif_rpm_verbosity_string_to_value(const gchar *value)
{
    if (g_strcmp0(value, "critical") == 0)
        return RPMLOG_CRIT;
    if (g_strcmp0(value, "emergency") == 0)
        return RPMLOG_EMERG;
    if (g_strcmp0(value, "error") == 0)
        return RPMLOG_ERR;
    if (g_strcmp0(value, "warn") == 0)
        return RPMLOG_WARNING;
    if (g_strcmp0(value, "debug") == 0)
        return RPMLOG_DEBUG;
    if (g_strcmp0(value, "info") == 0)
        return RPMLOG_INFO;
    return RPMLOG_EMERG;
}

/**
 * hif_transaction_delete_packages:
 **/
static gboolean
hif_transaction_delete_packages(HifTransaction *transaction,
                 HifState *state,
                 GError **error)
{
    HifTransactionPrivate *priv = GET_PRIVATE(transaction);
    HifState *state_local;
    HifPackage *pkg;
    const gchar *filename;
    const gchar *cachedir;
    guint i;

    /* nothing to delete? */
    if (priv->install->len == 0)
        return TRUE;

    /* get the cachedir so we only delete packages in the actual
     * cache, not local-install packages */
    cachedir = hif_context_get_cache_dir(priv->context);
    if (cachedir == NULL) {
        g_set_error_literal(error,
                            HIF_ERROR,
                            HIF_ERROR_FAILED_CONFIG_PARSING,
                            "Failed to get value for CacheDir");
        return FALSE;
    }

    /* delete each downloaded file */
    state_local = hif_state_get_child(state);
    hif_state_set_number_steps(state_local, priv->install->len);
    for (i = 0; i < priv->install->len; i++) {
        pkg = g_ptr_array_index(priv->install, i);

        /* don't delete files not in the repo */
        filename = hif_package_get_filename(pkg);
        if (g_str_has_prefix(filename, cachedir)) {
            g_autoptr(GFile) file = NULL;
            file = g_file_new_for_path(filename);
            if (!g_file_delete(file, NULL, error))
                return FALSE;
        }

        /* done */
        if (!hif_state_done(state_local, error))
            return FALSE;
    }
    return TRUE;
}

static gchar *
hif_transaction_get_propagated_reason(HifTransaction *transaction,
                                      HyGoal goal,
                                      HifPackage *pkg)
{
    HifTransactionPrivate *priv = GET_PRIVATE(transaction);

    /* kernel is always marked as "user" */
    if (hif_package_is_installonly(pkg))
        return g_strdup("user");

    /* for updates, propagate updated package's reason from existing yumdb entry */
    if (hif_package_get_action(pkg) == HIF_STATE_ACTION_DOWNGRADE ||
        hif_package_get_action(pkg) == HIF_STATE_ACTION_REINSTALL ||
        hif_package_get_action(pkg) == HIF_STATE_ACTION_UPDATE) {
        HifPackage *erased_package;

        erased_package = g_hash_table_lookup(priv->erased_by_package_hash, hif_package_get_package_id(pkg));
        if (erased_package != NULL) {
            gchar *reason;

            reason = hif_db_get_string(priv->db, erased_package, "reason", NULL);
            if (reason != NULL) {
                g_debug("propagating yumdb reason %s from %s to %s",
                        reason,
                        hif_package_get_package_id(erased_package),
                        hif_package_get_package_id(pkg));
                return reason;
            }
        }
        return g_strdup("dep");
    }

    /* for non-upgrades, packages explicitly passed to hawkey for
     * installation get "user" and dependencies it adds on its own get
     * "dep" */
    if (hy_goal_get_reason(goal, pkg) == HY_REASON_USER)
        return g_strdup("user");
    else
        return g_strdup("dep");
}

/**
 * hif_transaction_write_yumdb_install_item:
 *
 * We've used hif_package_set_pkgid() when running the transaction so we can
 * avoid the lookup in the rpmdb.
 **/
static gboolean
hif_transaction_write_yumdb_install_item(HifTransaction *transaction,
                                         HyGoal goal,
                                         HifPackage *pkg,
                                         HifState *state,
                                         GError **error)
{
    HifTransactionPrivate *priv = GET_PRIVATE(transaction);
    const gchar *tmp;
    g_autofree gchar *euid = NULL;
    g_autofree gchar *reason = NULL;

    /* should be set by hif_transaction_ts_progress_cb() */
    if (hif_package_get_pkgid(pkg) == NULL) {
        g_set_error(error,
                    HIF_ERROR,
                    HIF_ERROR_INTERNAL_ERROR,
                    "no yumdb entry for %s as no pkgid",
                    hif_package_get_package_id(pkg));
        return FALSE;
    }

    /* uncancellable */
    hif_state_set_allow_cancel(state, FALSE);

    /* set the repo this came from */
    tmp = hif_package_get_reponame(pkg);
    if (!hif_db_set_string(priv->db, pkg, "from_repo", tmp, error))
        return FALSE;

    /* write euid */
    euid = g_strdup_printf("%i", priv->uid);
    if (!hif_db_set_string(priv->db, pkg, "installed_by", euid, error))
        return FALSE;

    /* set the correct reason */
    reason = hif_transaction_get_propagated_reason(transaction, goal, pkg);
    if (!hif_db_set_string(priv->db, pkg, "reason", reason, error))
        return FALSE;

    /* set the correct release */
    tmp = hif_context_get_release_ver(priv->context);
    return hif_db_set_string(priv->db, pkg, "releasever", tmp, error);
}

/**
 * _hif_state_get_step_multiple_pair:
 *
 * 3,3    = 50
 * 3,0    = 99(we can't use 100 as an index value)
 * 0,3    = 1 (we can't use 0 as an index value)
 **/
static guint
_hif_state_get_step_multiple_pair(guint first, guint second)
{
    return 1 +(first * 98.0 /(first + second));
}

/**
 * hif_transaction_write_yumdb:
 **/
static gboolean
hif_transaction_write_yumdb(HifTransaction *transaction,
                 HyGoal goal,
                 HifState *state,
                 GError **error)
{
    HifState *state_local;
    HifState *state_loop;
    HifPackage *pkg;
    HifTransactionPrivate *priv = GET_PRIVATE(transaction);
    gboolean ret;
    guint i;
    guint steps_auto;

    steps_auto = _hif_state_get_step_multiple_pair(priv->install->len,
                                                   priv->remove->len + priv->remove_helper->len);
    ret = hif_state_set_steps(state,
                              error,
                              steps_auto,       /* install */
                              100 - steps_auto, /* remove */
                              -1);
    if (!ret)
        return FALSE;

    /* add all the new entries */
    state_local = hif_state_get_child(state);
    if (priv->install->len > 0)
        hif_state_set_number_steps(state_local,
                        priv->install->len);
    for (i = 0; i < priv->install->len; i++) {
        pkg = g_ptr_array_index(priv->install, i);
        state_loop = hif_state_get_child(state_local);
        ret = hif_transaction_write_yumdb_install_item(transaction,
                                                       goal,
                                                       pkg,
                                                       state_loop,
                                                       error);
        if (!ret)
            return FALSE;
        if (!hif_state_done(state_local, error))
            return FALSE;
    }

    /* this section done */
    if (!hif_state_done(state, error))
        return FALSE;

    /* remove all the old entries */
    if ((priv->remove->len + priv->remove_helper->len) > 0)
        hif_state_set_number_steps(state_local, priv->remove->len + priv->remove_helper->len);
    for (i = 0; i < priv->remove->len; i++) {
        pkg = g_ptr_array_index(priv->remove, i);
        if (!hif_transaction_ensure_repo(transaction, pkg, error))
            return FALSE;
        if (!hif_db_remove_all(priv->db, pkg, error))
            return FALSE;
        if (!hif_state_done(state_local, error))
            return FALSE;
    }
    for (i = 0; i < priv->remove_helper->len; i++) {
        pkg = g_ptr_array_index(priv->remove_helper, i);
        if (!hif_transaction_ensure_repo(transaction, pkg, error))
            return FALSE;
        if (!hif_db_remove_all(priv->db, pkg, error))
            return FALSE;
        if (!hif_state_done(state_local, error))
            return FALSE;
    }

    /* this section done */
    return hif_state_done(state, error);
}

static gboolean
hif_transaction_check_free_space(HifTransaction *transaction,
                                 GError **error)
{
    HifTransactionPrivate *priv = GET_PRIVATE(transaction);
    const gchar *cachedir;
    guint64 download_size;
    guint64 free_space;
    g_autoptr(GFile) file = NULL;
    g_autoptr(GFileInfo) filesystem_info = NULL;

    download_size = hif_package_array_get_download_size(priv->pkgs_to_download);

    cachedir = hif_context_get_cache_dir(priv->context);
    if (cachedir == NULL) {
        g_set_error_literal(error,
                            HIF_ERROR,
                            HIF_ERROR_FAILED_CONFIG_PARSING,
                            "Failed to get value for CacheDir");
        return FALSE;
    }

    file = g_file_new_for_path(cachedir);
    filesystem_info = g_file_query_filesystem_info(file, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, NULL, error);
    if (filesystem_info == NULL) {
        g_prefix_error(error, "Failed to get filesystem free size for %s: ", cachedir);
        return FALSE;
    }

    if (!g_file_info_has_attribute(filesystem_info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE)) {
        g_set_error(error,
                    HIF_ERROR,
                    HIF_ERROR_FAILED,
                    "Failed to get filesystem free size for %s",
                    cachedir);
        return FALSE;
    }

    free_space = g_file_info_get_attribute_uint64(filesystem_info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
    if (free_space < download_size) {
        g_autofree gchar *formatted_download_size = NULL;
        g_autofree gchar *formatted_free_size = NULL;

        formatted_download_size = g_format_size(download_size);
        formatted_free_size = g_format_size(free_space);
        g_set_error(error,
                    HIF_ERROR,
                    HIF_ERROR_NO_SPACE,
                    "Not enough free space in %s: needed %s, available %s",
                    cachedir,
                    formatted_download_size,
                    formatted_free_size);
        return FALSE;
    }

    return TRUE;
}

/**
 * hif_transaction_download:
 * @transaction: a #HifTransaction instance.
 * @state: A #HifState
 * @error: A #GError or %NULL
 *
 * Downloads all the packages needed for a transaction.
 *
 * Returns: %TRUE for success, %FALSE otherwise
 *
 * Since: 0.1.0
 **/
gboolean
hif_transaction_download(HifTransaction *transaction,
                         HifState *state,
                         GError **error)
{
    HifTransactionPrivate *priv = GET_PRIVATE(transaction);

    /* check that we have enough free space */
    if (!hif_transaction_check_free_space(transaction, error))
        return FALSE;

    /* just download the list */
    return hif_package_array_download(priv->pkgs_to_download,
                                      NULL,
                                      state,
                                      error);
}

/**
 * hif_transaction_depsolve:
 * @transaction: a #HifTransaction instance.
 * @goal: A #HyGoal
 * @state: A #HifState
 * @error: A #GError or %NULL
 *
 * Depsolves the transaction.
 *
 * Returns: %TRUE for success, %FALSE otherwise
 *
 * Since: 0.1.0
 **/
gboolean
hif_transaction_depsolve(HifTransaction *transaction,
                         HyGoal goal,
                         HifState *state,
                         GError **error)
{
    HifTransactionPrivate *priv = GET_PRIVATE(transaction);
    HifPackage *pkg;
    gboolean valid;
    guint i;
    g_autoptr(GPtrArray) packages = NULL;

    /* depsolve */
    if (!hif_goal_depsolve(goal, HIF_ALLOW_UNINSTALL, error))
        return FALSE;

    /* find a list of all the packages we have to download */
    g_ptr_array_set_size(priv->pkgs_to_download, 0);
    packages = hif_goal_get_packages(goal,
                                     HIF_PACKAGE_INFO_INSTALL,
                                     HIF_PACKAGE_INFO_REINSTALL,
                                     HIF_PACKAGE_INFO_DOWNGRADE,
                                     HIF_PACKAGE_INFO_UPDATE,
                                     -1);
    g_debug("Goal has %u packages", packages->len);
    for (i = 0; i < packages->len; i++) {
        pkg = g_ptr_array_index(packages, i);

        /* get correct package repo */
        if (!hif_transaction_ensure_repo(transaction, pkg, error))
            return FALSE;

        /* this is a local file */
        if (g_strcmp0(hif_package_get_reponame(pkg),
                      HY_CMDLINE_REPO_NAME) == 0) {
            continue;
        }

        /* check package exists and checksum is okay */
        if (!hif_package_check_filename(pkg, &valid, error))
            return FALSE;

        /* package needs to be downloaded */
        if (!valid) {
            g_ptr_array_add(priv->pkgs_to_download,
                            g_object_ref(pkg));
        }
    }
    return TRUE;
}

/**
 * hif_transaction_reset:
 */
static void
hif_transaction_reset(HifTransaction *transaction)
{
    HifTransactionPrivate *priv = GET_PRIVATE(transaction);

    /* reset */
    priv->child = NULL;
    g_ptr_array_set_size(priv->pkgs_to_download, 0);
    rpmtsEmpty(priv->ts);
    rpmtsSetNotifyCallback(priv->ts, NULL, NULL);

    /* clear */
    if (priv->install != NULL) {
        g_ptr_array_unref(priv->install);
        priv->install = NULL;
    }
    if (priv->remove != NULL) {
        g_ptr_array_unref(priv->remove);
        priv->remove = NULL;
    }
    if (priv->remove_helper != NULL) {
        g_ptr_array_unref(priv->remove_helper);
        priv->remove_helper = NULL;
    }
    if (priv->erased_by_package_hash != NULL) {
        g_hash_table_unref(priv->erased_by_package_hash);
        priv->erased_by_package_hash = NULL;
    }
}

/**
 * hif_transaction_commit:
 * @transaction: a #HifTransaction instance.
 * @goal: A #HyGoal
 * @state: A #HifState
 * @error: A #GError or %NULL
 *
 * Commits a transaction by installing and removing packages.
 *
 * NOTE: If this fails, you need to call hif_transaction_depsolve() again.
 *
 * Returns: %TRUE for success, %FALSE otherwise
 *
 * Since: 0.1.0
 **/
gboolean
hif_transaction_commit(HifTransaction *transaction,
                       HyGoal goal,
                       HifState *state,
                       GError **error)
{
    const gchar *filename;
    const gchar *tmp;
    gboolean allow_untrusted;
    gboolean is_update;
    gboolean ret = FALSE;
    gint rc;
    gint verbosity;
    gint vs_flags;
    guint i;
    guint j;
    HifState *state_local;
    GPtrArray *all_obsoleted;
    GPtrArray *pkglist;
    HifPackage *pkg;
    HifPackage *pkg_tmp;
    rpmprobFilterFlags problems_filter = 0;
    rpmtransFlags rpmts_flags = RPMTRANS_FLAG_NONE;
    HifTransactionPrivate *priv = GET_PRIVATE(transaction);

    /* take lock */
    ret = hif_state_take_lock(state,
                              HIF_LOCK_TYPE_RPMDB,
                              HIF_LOCK_MODE_PROCESS,
                              error);
    if (!ret)
        goto out;

    /* set state */
    if (priv->flags & HIF_TRANSACTION_FLAG_TEST) {
        ret = hif_state_set_steps(state,
                                  error,
                                  2, /* install */
                                  2, /* remove */
                                  10, /* test-commit */
                                  86, /* commit */
                                  -1);
    } else {
        ret = hif_state_set_steps(state,
                                  error,
                                  2, /* install */
                                  2, /* remove */
                                  10, /* test-commit */
                                  83, /* commit */
                                  1, /* write yumDB */
                                  2, /* delete files */
                                  -1);
    }
    if (!ret)
        goto out;

    /* import all GPG keys */
    ret = hif_keyring_add_public_keys(priv->keyring, error);
    if (!ret)
        goto out;

    /* find any packages without valid GPG signatures */
    ret = hif_transaction_check_untrusted(transaction, goal, error);
    if (!ret)
        goto out;

    hif_state_action_start(state, HIF_STATE_ACTION_REQUEST, NULL);

    /* get verbosity from the config file */
    tmp = hif_context_get_rpm_verbosity(priv->context);
    verbosity = hif_rpm_verbosity_string_to_value(tmp);
    rpmSetVerbosity(verbosity);

    /* setup the transaction */
    tmp = hif_context_get_install_root(priv->context);
    rc = rpmtsSetRootDir(priv->ts, tmp);
    if (rc < 0) {
        ret = FALSE;
        g_set_error_literal(error,
                            HIF_ERROR,
                            HIF_ERROR_INTERNAL_ERROR,
                            "failed to set root");
        goto out;
    }
    rpmtsSetNotifyCallback(priv->ts,
                           hif_transaction_ts_progress_cb,
                           transaction);

    /* add things to install */
    state_local = hif_state_get_child(state);
    priv->install = hif_goal_get_packages(goal,
                                          HIF_PACKAGE_INFO_INSTALL,
                                          HIF_PACKAGE_INFO_REINSTALL,
                                          HIF_PACKAGE_INFO_DOWNGRADE,
                                          HIF_PACKAGE_INFO_UPDATE,
                                          -1);
    if (priv->install->len > 0)
        hif_state_set_number_steps(state_local,
                                   priv->install->len);
    for (i = 0; i < priv->install->len; i++) {

        pkg = g_ptr_array_index(priv->install, i);
        ret = hif_transaction_ensure_repo(transaction, pkg, error);
        if (!ret)
            goto out;

        /* add the install */
        filename = hif_package_get_filename(pkg);
        allow_untrusted =(priv->flags & HIF_TRANSACTION_FLAG_ONLY_TRUSTED) == 0;
        is_update = hif_package_get_action(pkg) == HIF_STATE_ACTION_UPDATE ||
                hif_package_get_action(pkg) == HIF_STATE_ACTION_DOWNGRADE;
        ret = hif_rpmts_add_install_filename(priv->ts,
                                             filename,
                                             allow_untrusted,
                                             is_update,
                                             error);
        if (!ret)
            goto out;

        /* this section done */
        ret = hif_state_done(state_local, error);
        if (!ret)
            goto out;
    }

    /* this section done */
    ret = hif_state_done(state, error);
    if (!ret)
        goto out;

    /* add things to remove */
    priv->remove = hif_goal_get_packages(goal,
                                         HIF_PACKAGE_INFO_OBSOLETE,
                                         HIF_PACKAGE_INFO_REMOVE,
                                         -1);
    for (i = 0; i < priv->remove->len; i++) {
        pkg = g_ptr_array_index(priv->remove, i);
        ret = hif_rpmts_add_remove_pkg(priv->ts, pkg, error);
        if (!ret)
            goto out;

        /* pre-get the pkgid, as this isn't possible to get after
         * the sack is invalidated */
        if (hif_package_get_pkgid(pkg) == NULL) {
            g_warning("failed to pre-get pkgid for %s",
                      hif_package_get_package_id(pkg));
        }

        /* are the things being removed actually being upgraded */
        pkg_tmp = hif_find_pkg_from_name(priv->install,
                                         hif_package_get_name(pkg));
        if (pkg_tmp != NULL)
            hif_package_set_action(pkg, HIF_STATE_ACTION_CLEANUP);
    }

    /* add anything that gets obsoleted to a helper array which is used to
     * map removed packages auto-added by rpm to actual HifPackage's */
    priv->remove_helper = g_ptr_array_new_with_free_func((GDestroyNotify) g_object_unref);
    for (i = 0; i < priv->install->len; i++) {
        pkg = g_ptr_array_index(priv->install, i);
        is_update = hif_package_get_action(pkg) == HIF_STATE_ACTION_UPDATE ||
                    hif_package_get_action(pkg) == HIF_STATE_ACTION_DOWNGRADE;
        if (!is_update)
            continue;
        pkglist = hy_goal_list_obsoleted_by_package(goal, pkg);
        for (j = 0; j < pkglist->len; j++) {
            pkg_tmp = g_ptr_array_index (pkglist, i);
            g_ptr_array_add(priv->remove_helper,
                            g_object_ref(pkg_tmp));
            hif_package_set_action(pkg_tmp, HIF_STATE_ACTION_CLEANUP);
        }
        g_ptr_array_unref(pkglist);
    }

    /* this section done */
    ret = hif_state_done(state, error);
    if (!ret)
        goto out;

    /* map updated packages to their previous versions */
    priv->erased_by_package_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                         g_free, (GDestroyNotify) g_object_unref);
    all_obsoleted = hy_goal_list_obsoleted(goal, NULL);
    for (i = 0; i < priv->install->len; i++) {
        pkg = g_ptr_array_index(priv->install, i);
        if (hif_package_get_action(pkg) != HIF_STATE_ACTION_UPDATE &&
            hif_package_get_action(pkg) != HIF_STATE_ACTION_DOWNGRADE &&
            hif_package_get_action(pkg) != HIF_STATE_ACTION_REINSTALL)
            continue;

        pkglist = hy_goal_list_obsoleted_by_package(goal, pkg);
        for (j = 0; j < pkglist->len; j++) {
            pkg_tmp = g_ptr_array_index(pkglist, i);
            if (!hy_packagelist_has(all_obsoleted, pkg_tmp)) {
                g_hash_table_insert(priv->erased_by_package_hash,
                                    g_strdup(hif_package_get_package_id(pkg)),
                                    g_object_ref(pkg_tmp));
            }
        }
        g_ptr_array_unref(pkglist);
    }
    g_ptr_array_unref(all_obsoleted);

    /* generate ordering for the transaction */
    rpmtsOrder(priv->ts);

    /* run the test transaction */
    if (hif_context_get_check_transaction(priv->context)) {
        g_debug("running test transaction");
        hif_state_action_start(state,
                               HIF_STATE_ACTION_TEST_COMMIT,
                               NULL);
        priv->state = hif_state_get_child(state);
        priv->step = HIF_TRANSACTION_STEP_IGNORE;
        /* the output value of rpmtsCheck is not meaningful */
        rpmtsCheck(priv->ts);
        hif_state_action_stop(state);
        ret = hif_rpmts_look_for_problems(priv->ts, error);
        if (!ret)
            goto out;
    }

    /* this section done */
    ret = hif_state_done(state, error);
    if (!ret)
        goto out;

    /* no signature checking, we've handled that already */
    vs_flags = rpmtsSetVSFlags(priv->ts,
                               _RPMVSF_NOSIGNATURES | _RPMVSF_NODIGESTS);
    rpmtsSetVSFlags(priv->ts, vs_flags);

    /* filter diskspace */
    if (!hif_context_get_check_disk_space(priv->context))
        problems_filter |= RPMPROB_FILTER_DISKSPACE;
    if (priv->flags & HIF_TRANSACTION_FLAG_ALLOW_REINSTALL)
        problems_filter |= RPMPROB_FILTER_REPLACEPKG;
    if (priv->flags & HIF_TRANSACTION_FLAG_ALLOW_DOWNGRADE)
        problems_filter |= RPMPROB_FILTER_OLDPACKAGE;

    if (priv->flags & HIF_TRANSACTION_FLAG_NODOCS)
        rpmts_flags |= RPMTRANS_FLAG_NODOCS;

    if (priv->flags & HIF_TRANSACTION_FLAG_TEST) {
        /* run the transaction in test mode */
        rpmts_flags |= RPMTRANS_FLAG_TEST;

        priv->state = hif_state_get_child(state);
        priv->step = HIF_TRANSACTION_STEP_IGNORE;
        rpmtsSetFlags(priv->ts, rpmts_flags);
        g_debug("Running transaction in test mode");
        hif_state_set_allow_cancel(state, FALSE);
        rc = rpmtsRun(priv->ts, NULL, problems_filter);
        if (rc < 0) {
            ret = FALSE;
            g_set_error(error,
                        HIF_ERROR,
                        HIF_ERROR_INTERNAL_ERROR,
                        "Error %i running transaction test", rc);
            goto out;
        }
        if (rc > 0) {
            ret = hif_rpmts_look_for_problems(priv->ts, error);
            if (!ret)
                goto out;
        }

        /* transaction test done; return */
        ret = hif_state_done(state, error);
        goto out;
    }

    /* run the transaction */
    priv->state = hif_state_get_child(state);
    priv->step = HIF_TRANSACTION_STEP_STARTED;
    rpmtsSetFlags(priv->ts, rpmts_flags);
    g_debug("Running actual transaction");
    hif_state_set_allow_cancel(state, FALSE);
    rc = rpmtsRun(priv->ts, NULL, problems_filter);
    if (rc < 0) {
        ret = FALSE;
        g_set_error(error,
                    HIF_ERROR,
                    HIF_ERROR_INTERNAL_ERROR,
                    "Error %i running transaction", rc);
        goto out;
    }
    if (rc > 0) {
        ret = hif_rpmts_look_for_problems(priv->ts, error);
        if (!ret)
            goto out;
    }

    /* hmm, nothing was done... */
    if (priv->step != HIF_TRANSACTION_STEP_WRITING) {
        ret = FALSE;
        g_set_error(error,
                    HIF_ERROR,
                    HIF_ERROR_INTERNAL_ERROR,
                    "Transaction did not go to writing phase, "
                    "but returned no error(%i)",
                    priv->step);
        goto out;
    }

    /* this section done */
    ret = hif_state_done(state, error);
    if (!ret)
        goto out;

    /* write to the yumDB */
    state_local = hif_state_get_child(state);
    ret = hif_transaction_write_yumdb(transaction,
                                      goal,
                                      state_local,
                                      error);
    if (!ret)
        goto out;

    /* this section done */
    ret = hif_state_done(state, error);
    if (!ret)
        goto out;

    /* remove the files we downloaded */
    if (!hif_context_get_keep_cache(priv->context)) {
        state_local = hif_state_get_child(state);
        ret = hif_transaction_delete_packages(transaction,
                                              state_local,
                                              error);
        if (!ret)
            goto out;
    }

    /* all sacks are invalid now */
    hif_context_invalidate_full(priv->context, "transaction performed",
                                HIF_CONTEXT_INVALIDATE_FLAG_RPMDB |
                                HIF_CONTEXT_INVALIDATE_FLAG_ENROLLMENT);

    /* this section done */
    ret = hif_state_done(state, error);
    if (!ret)
        goto out;
out:
    hif_transaction_reset(transaction);
    hif_state_release_locks(state);
    return ret;
}

/**
 * hif_transaction_new:
 * @context: a #HifContext instance.
 *
 * Creates a new #HifTransaction.
 *
 * Returns:(transfer full): a #HifTransaction
 *
 * Since: 0.1.0
 **/
HifTransaction *
hif_transaction_new(HifContext *context)
{
    HifTransaction *transaction;
    HifTransactionPrivate *priv;
    transaction = g_object_new(HIF_TYPE_TRANSACTION, NULL);
    priv = GET_PRIVATE(transaction);
    priv->context = context;
    g_object_add_weak_pointer(G_OBJECT(priv->context),(void **) &priv->context);
    priv->db = hif_db_new(context);
    /* propagate db enablement */
    hif_db_set_enabled(priv->db, hif_context_get_yumdb_enabled(context));
    return HIF_TRANSACTION(transaction);
}
