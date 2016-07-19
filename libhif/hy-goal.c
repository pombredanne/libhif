/*
 * Copyright (C) 2012-2014 Red Hat, Inc.
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

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

// libsolv
#include <solv/evr.h>
#include <solv/selection.h>
#include <solv/solver.h>
#include <solv/solverdebug.h>
#include <solv/testcase.h>
#include <solv/transaction.h>
#include <solv/util.h>

// hawkey
#include "hif-types.h"
#include "hy-goal-private.h"
#include "hy-iutil.h"
#include "hy-package-private.h"
#include "hy-query-private.h"
#include "hif-reldep-private.h"
#include "hy-repo-private.h"
#include "hif-sack-private.h"
#include "hif-goal.h"
#include "hif-package.h"
#include "hy-selector-private.h"
#include "hy-util.h"


struct _SolutionCallback {
    HyGoal goal;
    hy_solution_callback callback;
    void *callback_data;
};

struct InstallonliesSortCallback {
    Pool *pool;
    Id running_kernel;
};

static int
erase_flags2libsolv(int flags)
{
    int ret = 0;
    if (flags & HY_CLEAN_DEPS)
        ret |= SOLVER_CLEANDEPS;
    return ret;
}

static gboolean
protected_in_removals(HyGoal goal) {
    guint i = 0;
    gboolean ret = FALSE;
    if (goal->removal_of_protected != NULL)
        g_ptr_array_free(goal->removal_of_protected, TRUE);
    if (goal->protected == NULL)
        return FALSE;
    goal->removal_of_protected = hif_goal_get_packages(goal,
                                     HIF_PACKAGE_INFO_REMOVE,
                                     HIF_PACKAGE_INFO_OBSOLETE,
                                     -1);
    while (i < goal->removal_of_protected->len) {
        HifPackage *pkg = g_ptr_array_index(goal->removal_of_protected, i);

        if (MAPTST(goal->protected, hif_package_get_id(pkg))) {
            ret = TRUE;
            i++;
        } else {
            g_ptr_array_remove_index(goal->removal_of_protected, i);
        }
    }
    return ret;
}

static void
same_name_subqueue(Pool *pool, Queue *in, Queue *out)
{
    Id el = queue_pop(in);
    Id name = pool_id2solvable(pool, el)->name;
    queue_empty(out);
    queue_push(out, el);
    while (in->count &&
           pool_id2solvable(pool, in->elements[in->count - 1])->name == name)
        // reverses the order so packages are sorted by descending version
        queue_push(out, queue_pop(in));
}

static int
can_depend_on(Pool *pool, Solvable *sa, Id b)
{
    // return 0 iff a does not depend on anything from b
    Queue requires;
    int ret = 1;

    queue_init(&requires);
    solvable_lookup_idarray(sa, SOLVABLE_REQUIRES, &requires);
    for (int i = 0; i < requires.count; ++i) {
        Id req_dep = requires.elements[i];
        Id p, pp;

        FOR_PROVIDES(p, pp, req_dep)
            if (p == b)
                goto done;
    }

    ret = 0;
 done:
    queue_free(&requires);
    return ret;
}

static int
sort_packages(const void *ap, const void *bp, void *s_cb)
{
    Id a = *(Id*)ap;
    Id b = *(Id*)bp;
    Pool *pool = ((struct InstallonliesSortCallback*) s_cb)->pool;
    Id kernel = ((struct InstallonliesSortCallback*) s_cb)->running_kernel;
    Solvable *sa = pool_id2solvable(pool, a);
    Solvable *sb = pool_id2solvable(pool, b);

    /* if the names are different sort them differently, particular order does
       not matter as long as it's consistent. */
    int name_diff = sa->name - sb->name;
    if (name_diff)
        return name_diff;

    /* same name, if one is/depends on the running kernel put it last */
    if (kernel >= 0) {
        if (a == kernel || can_depend_on(pool, sa, kernel))
            return 1;
        if (b == kernel || can_depend_on(pool, sb, kernel))
            return -1;
    }

    return pool_evrcmp(pool, sa->evr, sb->evr, EVRCMP_COMPARE);
}

static int
limit_installonly_packages(HyGoal goal, Solver *solv, Queue *job)
{
    HifSack *sack = goal->sack;
    if (!hif_sack_get_installonly_limit(sack))
        return 0;

    Queue *onlies = hif_sack_get_installonly(sack);
    Pool *pool = hif_sack_get_pool(sack);
    int reresolve = 0;

    for (int i = 0; i < onlies->count; ++i) {
        Id p, pp;
        Queue q;
        queue_init(&q);

        FOR_PKG_PROVIDES(p, pp, onlies->elements[i])
            if (solver_get_decisionlevel(solv, p) > 0)
                queue_push(&q, p);
        if (q.count <= (int) hif_sack_get_installonly_limit(sack)) {
            queue_free(&q);
            continue;
        }

        struct InstallonliesSortCallback s_cb = {pool, hif_sack_running_kernel(sack)};
        solv_sort(q.elements, q.count, sizeof(q.elements[0]), sort_packages, &s_cb);
        Queue same_names;
        queue_init(&same_names);
        while (q.count > 0) {
            same_name_subqueue(pool, &q, &same_names);
            if (same_names.count <= (int) hif_sack_get_installonly_limit(sack))
                continue;
            reresolve = 1;
            for (int j = 0; j < same_names.count; ++j) {
                Id id  = same_names.elements[j];
                Id action = SOLVER_ERASE;
                if (j < (int) hif_sack_get_installonly_limit(sack))
                    action = SOLVER_INSTALL;
                queue_push2(job, action | SOLVER_SOLVABLE, id);
            }
        }
        queue_free(&same_names);
        queue_free(&q);
    }
    return reresolve;
}

static int
internal_solver_callback(Solver *solv, void *data)
{
    struct _SolutionCallback *s_cb = (struct _SolutionCallback*)data;
    HyGoal goal = s_cb->goal;

    assert(goal->solv == solv);
    assert(goal->trans == NULL);
    goal->trans = solver_create_transaction(solv);
    int ret = s_cb->callback(goal, s_cb->callback_data);
    transaction_free(goal->trans);
    goal->trans = NULL;
    return ret;
}

static Solver *
init_solver(HyGoal goal, HifGoalActions flags)
{
    Pool *pool = hif_sack_get_pool(goal->sack);
    Solver *solv = solver_create(pool);

    if (goal->solv)
        solver_free(goal->solv);
    goal->solv = solv;

    /* no vendor locking */
    solver_set_flag(solv, SOLVER_FLAG_ALLOW_VENDORCHANGE, 1);
    /* don't erase packages that are no longer in repo during distupgrade */
    solver_set_flag(solv, SOLVER_FLAG_KEEP_ORPHANS, 1);
    /* no arch change for forcebest */
    solver_set_flag(solv, SOLVER_FLAG_BEST_OBEY_POLICY, 1);
    /* support package splits via obsoletes */
    solver_set_flag(solv, SOLVER_FLAG_YUM_OBSOLETES, 1);

#if defined(LIBSOLV_FLAG_URPMREORDER)
    /* support urpm-like solution reordering */
    solver_set_flag(solv, SOLVER_FLAG_URPM_REORDER, 1);
#endif

    return solv;
}

static void
allow_uninstall_all_but_protected(HyGoal goal, Queue *job, HifGoalActions flags) {
    Pool *pool = hif_sack_get_pool(goal->sack);

    if (goal->protected == NULL) {
        goal->protected = g_malloc0(sizeof(Map));
        map_init(goal->protected, pool->nsolvables);
    } else
        map_grow(goal->protected, pool->nsolvables);

    assert(goal->protected != NULL);
    Id kernel = hif_sack_running_kernel(goal->sack);
    if (kernel > 0)
        MAPSET(goal->protected, kernel);

    if (HIF_ALLOW_UNINSTALL & flags)
        for (Id id = 1; id < pool->nsolvables; ++id) {
            Solvable *s = pool_id2solvable(pool, id);
            if (pool->installed == s->repo) {
                if (!MAPTST(goal->protected, id) && pool->installed == s->repo)
                    queue_push2(job, SOLVER_ALLOWUNINSTALL|SOLVER_SOLVABLE, id);
            }
        }
}

static int
solve(HyGoal goal, Queue *job, HifGoalActions flags,
      hy_solution_callback user_cb, void * user_cb_data) {
    HifSack *sack = goal->sack;
    struct _SolutionCallback cb_tuple;

    /* apply the excludes */
    hif_sack_recompute_considered(sack);

    repo_internalize_all_trigger(hif_sack_get_pool(sack));
    hif_sack_make_provides_ready(sack);
    if (goal->trans) {
        transaction_free(goal->trans);
        goal->trans = NULL;
    }

    Solver *solv = init_solver(goal, flags);
    if (user_cb) {
        cb_tuple = (struct _SolutionCallback){goal, user_cb, user_cb_data};
        solv->solution_callback = internal_solver_callback;
        solv->solution_callback_data = &cb_tuple;
    }

    if (HIF_IGNORE_WEAK_DEPS & flags)
        solver_set_flag(solv, SOLVER_FLAG_IGNORE_RECOMMENDED, 1);

    if (solver_solve(solv, job))
        return 1;
    // either allow solutions callback or installonlies, both at the same time
    // are not supported
    if (!user_cb && limit_installonly_packages(goal, solv, job)) {
        // allow erasing non-installonly packages that depend on a kernel about
        // to be erased
        allow_uninstall_all_but_protected(goal, job, HIF_ALLOW_UNINSTALL);
        if (solver_solve(solv, job))
            return 1;
    }
    goal->trans = solver_create_transaction(solv);

    if (protected_in_removals(goal))
        return 1;

    return 0;
}

static Queue *
construct_job(HyGoal goal, HifGoalActions flags)
{
    HifSack *sack = goal->sack;
    Queue *job = g_malloc(sizeof(*job));

    queue_init_clone(job, &goal->staging);

    /* apply forcebest */
    if (flags & HIF_FORCE_BEST)
        for (int i = 0; i < job->count; i += 2)
            job->elements[i] |= SOLVER_FORCEBEST;

    /* turn off implicit obsoletes for installonly packages */
    for (int i = 0; i < (int) hif_sack_get_installonly(sack)->count; i++)
        queue_push2(job, SOLVER_MULTIVERSION|SOLVER_SOLVABLE_PROVIDES,
                    hif_sack_get_installonly(sack)->elements[i]);

    allow_uninstall_all_but_protected(goal, job, flags);

    if (flags & HIF_VERIFY)
        queue_push2(job, SOLVER_VERIFY|SOLVER_SOLVABLE_ALL, 0);

    return job;
}

static void
free_job(Queue *job)
{
    queue_free(job);
    g_free(job);
}

static GPtrArray *
list_results(HyGoal goal, Id type_filter1, Id type_filter2, GError **error)
{
    Queue transpkgs;
    Transaction *trans = goal->trans;
    GPtrArray *plist;

    /* no transaction */
    if (trans == NULL) {
        if (goal->solv == NULL) {
            g_set_error_literal (error,
                                 HIF_ERROR,
                                 HIF_ERROR_INTERNAL_ERROR,
                                 "no solv in the goal");
            return NULL;
        } else if (goal->removal_of_protected->len) {
            g_set_error_literal (error,
                                 HIF_ERROR,
                                 HIF_ERROR_REMOVAL_OF_PROTECTED_PKG,
                                 "no solution, cannot remove protected package");
            return NULL;
        }
        g_set_error_literal (error,
                             HIF_ERROR,
                             HIF_ERROR_NO_SOLUTION,
                             "no solution possible");
        return NULL;
    }
    queue_init(&transpkgs);
    plist = hy_packagelist_create();
    const int common_mode = SOLVER_TRANSACTION_SHOW_OBSOLETES |
        SOLVER_TRANSACTION_CHANGE_IS_REINSTALL;

    for (int i = 0; i < trans->steps.count; ++i) {
        Id p = trans->steps.elements[i];
        Id type;

        switch (type_filter1) {
        case SOLVER_TRANSACTION_OBSOLETED:
            type =  transaction_type(trans, p, common_mode);
            break;
        default:
            type  = transaction_type(trans, p, common_mode |
                                     SOLVER_TRANSACTION_SHOW_ACTIVE|
                                     SOLVER_TRANSACTION_SHOW_ALL);
            break;
        }

        if (type == type_filter1 || (type_filter2 && type == type_filter2))
            g_ptr_array_add(plist, hif_package_new(goal->sack, p));
    }
    return plist;
}

// internal functions to translate Selector into libsolv Job

static int
job_has(Queue *job, Id what, Id id)
{
    for (int i = 0; i < job->count; i += 2)
        if (job->elements[i] == what && job->elements[i + 1] == id)
            return 1;
    return 0;
}

static int
filter_arch2job(HifSack *sack, const struct _Filter *f, Queue *job)
{
    if (f == NULL)
        return 0;

    assert(f->cmp_type == HY_EQ);
    assert(f->nmatches == 1);
    Pool *pool = hif_sack_get_pool(sack);
    const char *arch = f->matches[0].str;
    Id archid = str2archid(pool, arch);

    if (archid == 0)
        return HIF_ERROR_INVALID_ARCHITECTURE;
    for (int i = 0; i < job->count; i += 2) {
        Id dep;
        assert((job->elements[i] & SOLVER_SELECTMASK) == SOLVER_SOLVABLE_NAME);
        dep = pool_rel2id(pool, job->elements[i + 1],
                          archid, REL_ARCH, 1);
        job->elements[i] |= SOLVER_SETARCH;
        job->elements[i + 1] = dep;
    }
    return 0;
}

static int
filter_evr2job(HifSack *sack, const struct _Filter *f, Queue *job)
{
    if (f == NULL)
        return 0;

    assert(f->cmp_type == HY_EQ);
    assert(f->nmatches == 1);

    Pool *pool = hif_sack_get_pool(sack);
    Id evr = pool_str2id(pool, f->matches[0].str, 1);
    Id constr = f->keyname == HY_PKG_VERSION ? SOLVER_SETEV : SOLVER_SETEVR;
    for (int i = 0; i < job->count; i += 2) {
        Id dep;
        assert((job->elements[i] & SOLVER_SELECTMASK) == SOLVER_SOLVABLE_NAME);
        dep = pool_rel2id(pool, job->elements[i + 1],
                          evr, REL_EQ, 1);
        job->elements[i] |= constr;
        job->elements[i + 1] = dep;
    }
    return 0;
}

static int
filter_file2job(HifSack *sack, const struct _Filter *f, Queue *job)
{
    if (f == NULL)
        return 0;
    assert(f->nmatches == 1);

    const char *file = f->matches[0].str;
    Pool *pool = hif_sack_get_pool(sack);

    int flags = f->cmp_type & HY_GLOB ? SELECTION_GLOB : 0;
    if (f->cmp_type & HY_GLOB)
        flags |= SELECTION_NOCASE;
    if (selection_make(pool, job, file, flags | SELECTION_FILELIST) == 0)
        return 1;
    return 0;
}

static int
filter_name2job(HifSack *sack, const struct _Filter *f, Queue *job)
{
    if (f == NULL)
        return 0;
    assert(f->nmatches == 1);

    Pool *pool = hif_sack_get_pool(sack);
    const char *name = f->matches[0].str;
    Id id;
    Dataiterator di;

    switch (f->cmp_type) {
    case HY_EQ:
        id = pool_str2id(pool, name, 0);
        if (id)
            queue_push2(job, SOLVER_SOLVABLE_NAME, id);
        break;
    case HY_GLOB:
        dataiterator_init(&di, pool, 0, 0, SOLVABLE_NAME, name, SEARCH_GLOB);
        while (dataiterator_step(&di)) {
            if (!is_package(pool, pool_id2solvable(pool, di.solvid)))
                continue;
            assert(di.idp);
            id = *di.idp;
            if (job_has(job, SOLVABLE_NAME, id))
                continue;
            queue_push2(job, SOLVER_SOLVABLE_NAME, id);
        }
        dataiterator_free(&di);
        break;
    default:
        assert(0);
        return 1;
    }
    return 0;
}

static int
filter_provides2job(HifSack *sack, const struct _Filter *f, Queue *job)
{
    if (f == NULL)
        return 0;
    assert(f->nmatches == 1);

    Pool *pool = hif_sack_get_pool(sack);
    const char *name = f->matches[0].str;
    Id id;
    Dataiterator di;

    switch (f->cmp_type) {
    case HY_EQ:
        id = hif_reldep_get_id (f->matches[0].reldep);
        queue_push2(job, SOLVER_SOLVABLE_PROVIDES, id);
        break;
    case HY_GLOB:
        dataiterator_init(&di, pool, 0, 0, SOLVABLE_PROVIDES, name, SEARCH_GLOB);
        while (dataiterator_step(&di)) {
            if (is_package(pool, pool_id2solvable(pool, di.solvid)))
                break;
        }
        assert(di.idp);
        id = *di.idp;
        if (!job_has(job, SOLVABLE_PROVIDES, id))
            queue_push2(job, SOLVER_SOLVABLE_PROVIDES, id);
        dataiterator_free(&di);
        break;
    default:
        assert(0);
        return 1;
    }
    return 0;
}

static int
filter_reponame2job(HifSack *sack, const struct _Filter *f, Queue *job)
{
    Queue repo_sel;
    Id i;
    Repo *repo;

    if (f == NULL)
        return 0;

    assert(f->cmp_type == HY_EQ);
    assert(f->nmatches == 1);

    queue_init(&repo_sel);
    Pool *pool = hif_sack_get_pool(sack);
    FOR_REPOS(i, repo)
        if (!strcmp(f->matches[0].str, repo->name)) {
            queue_push2(&repo_sel, SOLVER_SOLVABLE_REPO | SOLVER_SETREPO, repo->repoid);
        }

    selection_filter(pool, job, &repo_sel);

    queue_free(&repo_sel);
    return 0;
}

/**
 * Build job queue from a Query.
 *
 * Returns an error code
 */
int
sltr2job(const HySelector sltr, Queue *job, int solver_action)
{
    HifSack *sack = selector_sack(sltr);
    int ret = 0;
    Queue job_sltr;
    int any_opt_filter = sltr->f_arch || sltr->f_evr || sltr->f_reponame;
    int any_req_filter = sltr->f_name || sltr->f_provides || sltr->f_file;

    queue_init(&job_sltr);

    if (!any_req_filter) {
        if (any_opt_filter)
            // no name or provides or file in the selector is an error
            ret = HIF_ERROR_BAD_SELECTOR;
        goto finish;
    }

    hif_sack_recompute_considered(sack);
    hif_sack_make_provides_ready(sack);
    ret = filter_name2job(sack, sltr->f_name, &job_sltr);
    if (ret)
        goto finish;
    ret = filter_file2job(sack, sltr->f_file, &job_sltr);
    if (ret)
        goto finish;
    ret = filter_provides2job(sack, sltr->f_provides, &job_sltr);
    if (ret)
        goto finish;
    ret = filter_arch2job(sack, sltr->f_arch, &job_sltr);
    if (ret)
        goto finish;
    ret = filter_evr2job(sack, sltr->f_evr, &job_sltr);
    if (ret)
        goto finish;
    ret = filter_reponame2job(sack, sltr->f_reponame, &job_sltr);
    if (ret)
        goto finish;

    for (int i = 0; i < job_sltr.count; i += 2)
         queue_push2(job,
                     job_sltr.elements[i] | solver_action,
                     job_sltr.elements[i + 1]);

 finish:
    queue_free(&job_sltr);
    return ret;
}

// public functions

HyGoal
hy_goal_clone(HyGoal goal)
{
    HyGoal gn = hy_goal_create(goal->sack);
    queue_init_clone(&gn->staging, &goal->staging);
    gn->protected = g_malloc0(sizeof(Map));
    if (goal->protected != NULL)
        map_init_clone(gn->protected, goal->protected);
    gn->actions = goal->actions;
    gn->removal_of_protected = g_ptr_array_ref(goal->removal_of_protected);
    return gn;
}

HyGoal
hy_goal_create(HifSack *sack)
{
    HyGoal goal = g_malloc0(sizeof(*goal));
    goal->sack = sack;
    goal->removal_of_protected = g_ptr_array_new();
    queue_init(&goal->staging);
    return goal;
}

void
hy_goal_free(HyGoal goal)
{
    if (goal->trans)
        transaction_free(goal->trans);
    if (goal->solv)
        solver_free(goal->solv);
    queue_free(&goal->staging);
    free_map_fully(goal->protected);
    g_ptr_array_unref(goal->removal_of_protected);
    g_free(goal);
}

int
hy_goal_distupgrade_all(HyGoal goal)
{
    goal->actions |= HIF_DISTUPGRADE_ALL;
    queue_push2(&goal->staging, SOLVER_DISTUPGRADE|SOLVER_SOLVABLE_ALL, 0);
    return 0;
}

int
hy_goal_distupgrade(HyGoal goal, HifPackage *new_pkg)
{
    goal->actions |= HIF_DISTUPGRADE;
    queue_push2(&goal->staging, SOLVER_SOLVABLE|SOLVER_DISTUPGRADE,
        hif_package_get_id(new_pkg));
    return 0;
}

int
hy_goal_distupgrade_selector(HyGoal goal, HySelector sltr)
{
    goal->actions |= HIF_DISTUPGRADE;
    return sltr2job(sltr, &goal->staging, SOLVER_DISTUPGRADE);
}

int
hy_goal_downgrade_to(HyGoal goal, HifPackage *new_pkg)
{
    goal->actions |= HIF_DOWNGRADE;
    return hy_goal_install(goal, new_pkg);
}

int
hy_goal_erase(HyGoal goal, HifPackage *pkg)
{
    goal->actions |= HIF_ERASE;
    return hy_goal_erase_flags(goal, pkg, 0);
}

int
hy_goal_erase_flags(HyGoal goal, HifPackage *pkg, int flags)
{
#ifndef NDEBUG
    Pool *pool = hif_sack_get_pool(goal->sack);
    assert(pool->installed &&
           pool_id2solvable(pool, hif_package_get_id(pkg))->repo == pool->installed);
#endif
    int additional = erase_flags2libsolv(flags);
    goal->actions |= HIF_ERASE;
    queue_push2(&goal->staging, SOLVER_SOLVABLE|SOLVER_ERASE|additional,
                hif_package_get_id(pkg));
    return 0;
}

int
hy_goal_erase_selector(HyGoal goal, HySelector sltr)
{
    goal->actions |= HIF_ERASE;
    return hy_goal_erase_selector_flags(goal, sltr, 0);
}

int
hy_goal_erase_selector_flags(HyGoal goal, HySelector sltr, int flags)
{
    int additional = erase_flags2libsolv(flags);
    goal->actions |= HIF_ERASE;
    return sltr2job(sltr, &goal->staging, SOLVER_ERASE|additional);
}

int
hy_goal_has_actions(HyGoal goal, HifGoalActions action)
{
    return goal->actions & action;
}

int
hy_goal_install(HyGoal goal, HifPackage *new_pkg)
{
    goal->actions |= HIF_INSTALL;
    queue_push2(&goal->staging, SOLVER_SOLVABLE|SOLVER_INSTALL, hif_package_get_id(new_pkg));
    return 0;
}

int
hy_goal_install_optional(HyGoal goal, HifPackage *new_pkg)
{
    goal->actions |= HIF_INSTALL;
    queue_push2(&goal->staging, SOLVER_SOLVABLE|SOLVER_INSTALL|SOLVER_WEAK,
                hif_package_get_id(new_pkg));
    return 0;
}

gboolean
hy_goal_install_selector(HyGoal goal, HySelector sltr, GError **error)
{
    int rc;
    goal->actions |= HIF_INSTALL;
    rc = sltr2job(sltr, &goal->staging, SOLVER_INSTALL);
    if (rc != 0) {
        g_set_error_literal (error,
                             HIF_ERROR,
                             rc,
                             "failed to install selector");
        return FALSE;
    }
    return TRUE;
}

gboolean
hy_goal_install_selector_optional(HyGoal goal, HySelector sltr, GError **error)
{
    int rc;
    goal->actions |= HIF_INSTALL;
    rc = sltr2job(sltr, &goal->staging, SOLVER_INSTALL|SOLVER_WEAK);
    if (rc != 0) {
        g_set_error_literal (error,
                             HIF_ERROR,
                             rc,
                             "failed to install optional selector");
        return FALSE;
    }
    return TRUE;
}

int
hy_goal_upgrade_all(HyGoal goal)
{
    goal->actions |= HIF_UPGRADE_ALL;
    queue_push2(&goal->staging, SOLVER_UPDATE|SOLVER_SOLVABLE_ALL, 0);
    return 0;
}

int
hy_goal_upgrade_to(HyGoal goal, HifPackage *new_pkg)
{
    goal->actions |= HIF_UPGRADE;
    return hy_goal_upgrade_to_flags(goal, new_pkg, 0);
}

int
hy_goal_upgrade_to_selector(HyGoal goal, HySelector sltr)
{
    goal->actions |= HIF_UPGRADE;
    if (sltr->f_evr == NULL)
        return sltr2job(sltr, &goal->staging, SOLVER_UPDATE);
    return sltr2job(sltr, &goal->staging, SOLVER_INSTALL);
}

int
hy_goal_upgrade_selector(HyGoal goal, HySelector sltr)
{
    goal->actions |= HIF_UPGRADE;
    return sltr2job(sltr, &goal->staging, SOLVER_UPDATE);
}

int
hy_goal_upgrade_to_flags(HyGoal goal, HifPackage *new_pkg, int flags)
{
    int count = 0;

    if (flags & HY_CHECK_INSTALLED) {
        HyQuery q = hy_query_create(goal->sack);
        const char *name = hif_package_get_name(new_pkg);
        GPtrArray *installed;

        hy_query_filter(q, HY_PKG_NAME, HY_EQ, name);
        hy_query_filter(q, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
        installed = hy_query_run(q);
        count = installed->len;
        g_ptr_array_unref(installed);
        hy_query_free(q);
        if (!count)
            return HIF_ERROR_PACKAGE_NOT_FOUND;
    }
    goal->actions |= HIF_UPGRADE;

    return hy_goal_install(goal, new_pkg);
}

int
hy_goal_userinstalled(HyGoal goal, HifPackage *pkg)
{
    queue_push2(&goal->staging, SOLVER_SOLVABLE|SOLVER_USERINSTALLED,
                hif_package_get_id(pkg));
    return 0;
}

int hy_goal_req_length(HyGoal goal)
{
    return goal->staging.count / 2;
}

int
hy_goal_run(HyGoal goal)
{
    return hy_goal_run_flags(goal, 0);
}

int
hy_goal_run_flags(HyGoal goal, HifGoalActions flags)
{
    return hy_goal_run_all_flags(goal, NULL, NULL, flags);
}

int
hy_goal_run_all(HyGoal goal, hy_solution_callback cb, void *cb_data)
{
    return hy_goal_run_all_flags(goal, cb, cb_data, 0);
}

int
hy_goal_run_all_flags(HyGoal goal, hy_solution_callback cb, void *cb_data,
                      HifGoalActions flags)
{
    Queue *job = construct_job(goal, flags);
    goal->actions |= flags;
    int ret = solve(goal, job, flags, cb, cb_data);
    free_job(job);
    return ret;
}

int
hy_goal_count_problems(HyGoal goal)
{
    assert(goal->solv);
    return solver_problem_count(goal->solv) + MIN(1, goal->removal_of_protected->len);
}

/**
 * String describing the encountered solving problem 'i'.
 *
 * Caller is responsible for freeing the returned string using g_free().
 */
char *
hy_goal_describe_problem(HyGoal goal, unsigned i)
{
    Id rid, source, target, dep;
    SolverRuleinfo type;
    g_autoptr(GString) string = NULL;
    HifPackage *pkg;
    guint j;
    const char *name;

    /* internal error */
    if (i >= (unsigned) hy_goal_count_problems(goal))
        return NULL;
    // problem is not in libsolv - removal of protected packages
    if (i >= (unsigned) solver_problem_count(goal->solv)) {
        string = g_string_new("The operation would result in removing"
                              " the following protected packages: ");
        for (j = 0; j < goal->removal_of_protected->len; ++j) {
            pkg = g_ptr_array_index (goal->removal_of_protected, i);
            name = hif_package_get_name(pkg);
            if (j == 0) {
                g_string_append(string, name);
            } else {
                g_string_append_printf(string, ", %s", name);
            }
        }
        return g_strdup(string->str);
    }

    // this libsolv interface indexes from 1 (we do from 0), so:
    rid = solver_findproblemrule(goal->solv, i + 1);
    type = solver_ruleinfo(goal->solv, rid, &source, &target, &dep);

    const char *problem = solver_problemruleinfo2str(goal->solv,
                                                     type, source, target, dep);
    return g_strdup(problem);
}

/**
 * Write all the solving decisions to the hawkey logfile.
 */
int
hy_goal_log_decisions(HyGoal goal)
{
    if (goal->solv == NULL)
        return 1;
    solver_printdecisionq(goal->solv, SOLV_DEBUG_RESULT);
    return 0;
}

/**
 * hy_goal_write_debugdata:
 * @goal: A #HyGoal
 * @dir: The directory to write to
 * @error: A #GError, or %NULL
 *
 * Writes details about the testcase to a directory.
 *
 * Returns: %FALSE if an error was set
 *
 * Since: 0.7.0
 */
gboolean
hy_goal_write_debugdata(HyGoal goal, const char *dir, GError **error)
{
    Solver *solv = goal->solv;
    if (solv == NULL) {
        g_set_error_literal (error,
                             HIF_ERROR,
                             HIF_ERROR_INTERNAL_ERROR,
                             "no solver set");
        return FALSE;
    }

    int flags = TESTCASE_RESULT_TRANSACTION | TESTCASE_RESULT_PROBLEMS;
    g_autofree char *absdir = abspath(dir);
    if (absdir == NULL) {
        g_set_error (error,
                     HIF_ERROR,
                     HIF_ERROR_FILE_INVALID,
                     "failed to make %s absolute", dir);
        return FALSE;
    }
    g_debug("writing solver debugdata to %s", absdir);
    int ret = testcase_write(solv, absdir, flags, NULL, NULL);
    if (!ret) {
        g_set_error (error,
                     HIF_ERROR,
                     HIF_ERROR_FILE_INVALID,
                     "failed writing debugdata to %s: %s",
                     absdir, strerror(errno));
        return FALSE;
    }
    return TRUE;
}

GPtrArray *
hy_goal_list_erasures(HyGoal goal, GError **error)
{
    return list_results(goal, SOLVER_TRANSACTION_ERASE, 0, error);
}

GPtrArray *
hy_goal_list_installs(HyGoal goal, GError **error)
{
    return list_results(goal, SOLVER_TRANSACTION_INSTALL,
                        SOLVER_TRANSACTION_OBSOLETES, error);
}

GPtrArray *
hy_goal_list_obsoleted(HyGoal goal, GError **error)
{
    return list_results(goal, SOLVER_TRANSACTION_OBSOLETED, 0, error);
}

GPtrArray *
hy_goal_list_reinstalls(HyGoal goal, GError **error)
{
    return list_results(goal, SOLVER_TRANSACTION_REINSTALL, 0, error);
}

GPtrArray *
hy_goal_list_unneeded(HyGoal goal, GError **error)
{
    GPtrArray *plist = hy_packagelist_create();
    Queue q;
    Solver *solv = goal->solv;

    queue_init(&q);
    solver_get_unneeded(solv, &q, 0);
    queue2plist(goal->sack, &q, plist);
    queue_free(&q);
    return plist;
}

GPtrArray *
hy_goal_list_upgrades(HyGoal goal, GError **error)
{
    return list_results(goal, SOLVER_TRANSACTION_UPGRADE, 0, error);
}

GPtrArray *
hy_goal_list_downgrades(HyGoal goal, GError **error)
{
    return list_results(goal, SOLVER_TRANSACTION_DOWNGRADE, 0, error);
}

GPtrArray *
hy_goal_list_obsoleted_by_package(HyGoal goal, HifPackage *pkg)
{
    HifSack *sack = goal->sack;
    Transaction *trans = goal->trans;
    Queue obsoletes;
    GPtrArray *plist = hy_packagelist_create();

    assert(trans);
    queue_init(&obsoletes);

    transaction_all_obs_pkgs(trans, hif_package_get_id(pkg), &obsoletes);
    queue2plist(sack, &obsoletes, plist);

    queue_free(&obsoletes);
    return plist;
}

int
hy_goal_get_reason(HyGoal goal, HifPackage *pkg)
{
    assert(goal->solv);
    Id info;
    int reason = solver_describe_decision(goal->solv, hif_package_get_id(pkg), &info);

    if ((reason == SOLVER_REASON_UNIT_RULE ||
         reason == SOLVER_REASON_RESOLVE_JOB) &&
        solver_ruleclass(goal->solv, info) == SOLVER_RULE_JOB)
        return HY_REASON_USER;
    if (reason == SOLVER_REASON_CLEANDEPS_ERASE)
        return HY_REASON_CLEAN;
    if (reason == SOLVER_REASON_WEAKDEP)
        return HY_REASON_WEAKDEP;
    return HY_REASON_DEP;
}
