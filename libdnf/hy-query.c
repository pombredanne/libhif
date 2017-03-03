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

#define _GNU_SOURCE
#include <glib.h>
#include <assert.h>
#include <fnmatch.h>
#include <string.h>
#include <solv/evr.h>
#include <solv/repo.h>
#include <solv/solver.h>
#include <solv/util.h>

#include "dnf-types.h"
#include "dnf-advisorypkg.h"
#include "hy-iutil.h"
#include "hy-query-private.h"
#include "hy-package-private.h"
#include "hy-packageset-private.h"
#include "dnf-reldep-private.h"
#include "dnf-sack-private.h"
#include "hy-util.h"

#define BLOCK_SIZE 15

static int
match_type_num(int keyname) {
    switch (keyname) {
    case HY_PKG_EPOCH:
        return 1;
    default:
        return 0;
    }
}

static int
match_type_pkg(int keyname) {
    switch (keyname) {
    case HY_PKG:
    case HY_PKG_OBSOLETES:
        return 1;
    default:
        return 0;
    }
}

static int
match_type_reldep(int keyname) {
    switch (keyname) {
    case HY_PKG_CONFLICTS:
    case HY_PKG_ENHANCES:
    case HY_PKG_OBSOLETES:
    case HY_PKG_PROVIDES:
    case HY_PKG_RECOMMENDS:
    case HY_PKG_REQUIRES:
    case HY_PKG_SUGGESTS:
    case HY_PKG_SUPPLEMENTS:
        return 1;
    default:
        return 0;
    }
}

static int
match_type_str(int keyname) {
    switch (keyname) {
    case HY_PKG_ADVISORY:
    case HY_PKG_ADVISORY_BUG:
    case HY_PKG_ADVISORY_CVE:
    case HY_PKG_ADVISORY_SEVERITY:
    case HY_PKG_ADVISORY_TYPE:
    case HY_PKG_ARCH:
    case HY_PKG_DESCRIPTION:
    case HY_PKG_ENHANCES:
    case HY_PKG_EVR:
    case HY_PKG_FILE:
    case HY_PKG_LOCATION:
    case HY_PKG_NAME:
    case HY_PKG_NEVRA:
    case HY_PKG_PROVIDES:
    case HY_PKG_RECOMMENDS:
    case HY_PKG_RELEASE:
    case HY_PKG_REPONAME:
    case HY_PKG_REQUIRES:
    case HY_PKG_SOURCERPM:
    case HY_PKG_SUGGESTS:
    case HY_PKG_SUMMARY:
    case HY_PKG_SUPPLEMENTS:
    case HY_PKG_OBSOLETES:
    case HY_PKG_CONFLICTS:
    case HY_PKG_URL:
    case HY_PKG_VERSION:
        return 1;
    default:
        return 0;
    }
}


static Id
di_keyname2id(int keyname)
{
    switch(keyname) {
    case HY_PKG_DESCRIPTION:
        return SOLVABLE_DESCRIPTION;
    case HY_PKG_NAME:
        return SOLVABLE_NAME;
    case HY_PKG_URL:
        return SOLVABLE_URL;
    case HY_PKG_ARCH:
        return SOLVABLE_ARCH;
    case HY_PKG_EVR:
        return SOLVABLE_EVR;
    case HY_PKG_SUMMARY:
        return SOLVABLE_SUMMARY;
    case HY_PKG_FILE:
        return SOLVABLE_FILELIST;
    default:
        assert(0);
        return 0;
    }
}

static Id
reldep_keyname2id(int keyname)
{
    switch(keyname) {
    case HY_PKG_CONFLICTS:
        return SOLVABLE_CONFLICTS;
    case HY_PKG_ENHANCES:
        return SOLVABLE_ENHANCES;
    case HY_PKG_OBSOLETES:
        return SOLVABLE_OBSOLETES;
    case HY_PKG_REQUIRES:
        return SOLVABLE_REQUIRES;
    case HY_PKG_RECOMMENDS:
        return SOLVABLE_RECOMMENDS;
    case HY_PKG_SUGGESTS:
        return SOLVABLE_SUGGESTS;
    case HY_PKG_SUPPLEMENTS:
        return SOLVABLE_SUPPLEMENTS;
    default:
        assert(0);
        return 0;
    }
}

static int
type2flags(int type, int keyname)
{
    int ret = 0;
    if (keyname == HY_PKG_FILE)
        ret |= SEARCH_FILES | SEARCH_COMPLETE_FILELIST;
    if (type & HY_ICASE)
        ret |= SEARCH_NOCASE;

    type &= ~HY_COMPARISON_FLAG_MASK;
    switch (type) {
    case HY_EQ:
        return ret | SEARCH_STRING;
    case HY_SUBSTR:
        return ret | SEARCH_SUBSTRING;
    case HY_GLOB:
        return ret | SEARCH_GLOB;
    default:
        assert(0); // not implemented
        return 0;
    }
}

static int
valid_filter_str(int keyname, int cmp_type)
{
    if (!match_type_str(keyname))
        return 0;

    cmp_type &= ~HY_NOT; // hy_query_run always handles NOT
    switch (keyname) {
    case HY_PKG_LOCATION:
    case HY_PKG_SOURCERPM:
        return cmp_type == HY_EQ;
    default:
        return 1;
    }
}

static int
valid_filter_num(int keyname, int cmp_type)
{
    if (!match_type_num(keyname))
        return 0;

    cmp_type &= ~HY_NOT; // hy_query_run always handles NOT
    if (cmp_type & (HY_ICASE | HY_SUBSTR | HY_GLOB))
        return 0;
    switch (keyname) {
    case HY_PKG:
        return cmp_type == HY_EQ;
    default:
        return 1;
    }
}

static int
valid_filter_pkg(int keyname, int cmp_type)
{
    if (!match_type_pkg(keyname))
        return 0;
    return cmp_type == HY_EQ || cmp_type == HY_NEQ;
}

static int
valid_filter_reldep(int keyname)
{
    if (!match_type_reldep(keyname))
        return 0;
    return 1;
}

struct _Filter *
filter_create(int nmatches)
{
    struct _Filter *f = g_malloc0(sizeof(struct _Filter));
    filter_reinit(f, nmatches);
    return f;
}

void
filter_reinit(struct _Filter *f, int nmatches)
{
    for (int m = 0; m < f->nmatches; ++m)
        switch (f->match_type) {
        case _HY_PKG:
            g_object_unref(f->matches[m].pset);
            break;
        case _HY_STR:
            g_free(f->matches[m].str);
            break;
        case _HY_RELDEP:
            if (f->matches[m].reldep)
                g_object_unref (f->matches[m].reldep);
            break;
        default:
            break;
        }
    g_free(f->matches);
    f->match_type = _HY_VOID;
    if (nmatches > 0)
        f->matches = g_malloc0(nmatches * sizeof(union _Match *));
    else
        f->matches = NULL;
    f->nmatches = nmatches;
}

void
filter_free(struct _Filter *f)
{
    if (f) {
        filter_reinit(f, 0);
        g_free(f);
    }
}

static struct _Filter *
query_add_filter(HyQuery q, int nmatches)
{
    struct _Filter filter;
    memset(&filter, 0, sizeof(filter));
    filter_reinit(&filter, nmatches);
    q->filters = solv_extend(q->filters, q->nfilters, 1, sizeof(filter),
                             BLOCK_SIZE);
    q->filters[q->nfilters] = filter; /* structure assignment */
    return q->filters + q->nfilters++;
}

static void
filter_dataiterator(HyQuery q, struct _Filter *f, Map *m)
{
    Pool *pool = dnf_sack_get_pool(q->sack);
    Dataiterator di;
    Id keyname = di_keyname2id(f->keyname);
    int flags = type2flags(f->cmp_type, f->keyname);

    assert(f->match_type == _HY_STR);
    /* do an OR over all matches: */
    for (int i = 0; i < f->nmatches; ++i) {
        dataiterator_init(&di, pool, 0, 0,
                          keyname,
                          f->matches[i].str,
                          flags);
        while (dataiterator_step(&di))
            MAPSET(m, di.solvid);
        dataiterator_free(&di);
    }
}

static void
filter_pkg(HyQuery q, struct _Filter *f, Map *m)
{
    assert(f->nmatches == 1);
    assert(f->match_type == _HY_PKG);

    map_free(m);
    map_init_clone(m, dnf_packageset_get_map(f->matches[0].pset));
}

static void
filter_all(HyQuery q, struct _Filter *f, Map *m)
{
    assert(f->nmatches == 1);
    assert(f->match_type == _HY_NUM);
    assert(f->cmp_type == HY_EQ);
    assert(f->matches[0].num == -1);
    // just leaves m empty
}

static void
filter_epoch(HyQuery q, struct _Filter *f, Map *m)
{
    Pool *pool = dnf_sack_get_pool(q->sack);

    for (int mi = 0; mi < f->nmatches; ++mi) {
        unsigned long epoch = f->matches[mi].num;

        for (Id id = 1; id < pool->nsolvables; ++id) {
            if (!MAPTST(q->result, id))
                continue;
            Solvable *s = pool_id2solvable(pool, id);
            if (s->evr == ID_EMPTY)
                continue;

            const char *evr = pool_id2str(pool, s->evr);
            unsigned long pkg_epoch = pool_get_epoch(pool, evr);

            int cmp_type = f->cmp_type;
            if ((pkg_epoch > epoch && cmp_type & HY_GT) ||
                (pkg_epoch < epoch && cmp_type & HY_LT) ||
                (pkg_epoch == epoch && cmp_type & HY_EQ))
                MAPSET(m, id);
        }
    }
}

static void
filter_evr(HyQuery q, struct _Filter *f, Map *m)
{
    Pool *pool = dnf_sack_get_pool(q->sack);

    for (int mi = 0; mi < f->nmatches; ++mi) {
        Id match_evr = pool_str2id(pool, f->matches[mi].str, 1);

        for (Id id = 1; id < pool->nsolvables; ++id) {
            if (!MAPTST(q->result, id))
                continue;
            Solvable *s = pool_id2solvable(pool, id);
            int cmp = pool_evrcmp(pool, s->evr, match_evr, EVRCMP_COMPARE);

            if ((cmp > 0 && f->cmp_type & HY_GT) ||
                (cmp < 0 && f->cmp_type & HY_LT) ||
                (cmp == 0 && f->cmp_type & HY_EQ))
                MAPSET(m, id);
        }
    }
}

static void
filter_version(HyQuery q, struct _Filter *f, Map *m)
{
    Pool *pool = dnf_sack_get_pool(q->sack);
    int cmp_type = f->cmp_type;

    for (int mi = 0; mi < f->nmatches; ++mi) {
        const char *match = f->matches[mi].str;
        char *filter_vr = solv_dupjoin(match, "-0", NULL);

        for (Id id = 1; id < pool->nsolvables; ++id) {
            if (!MAPTST(q->result, id))
                continue;
            char *e, *v, *r;
            Solvable *s = pool_id2solvable(pool, id);
            if (s->evr == ID_EMPTY)
                continue;
            const char *evr = pool_id2str(pool, s->evr);

            pool_split_evr(pool, evr, &e, &v, &r);

            if (cmp_type == HY_GLOB) {
                if (fnmatch(match, v, 0))
                    continue;
                MAPSET(m, id);
            }

            char *vr = pool_tmpjoin(pool, v, "-0", NULL);
            int cmp = pool_evrcmp_str(pool, vr, filter_vr, EVRCMP_COMPARE);
            if ((cmp > 0 && cmp_type & HY_GT) ||
                (cmp < 0 && cmp_type & HY_LT) ||
                (cmp == 0 && cmp_type & HY_EQ))
                MAPSET(m, id);
        }
        g_free(filter_vr);
    }
}

static void
filter_release(HyQuery q, struct _Filter *f, Map *m)
{
    Pool *pool = dnf_sack_get_pool(q->sack);
    int cmp_type = f->cmp_type;

    for (int mi = 0; mi < f->nmatches; ++mi) {
        const char *match = f->matches[mi].str;
        char *filter_vr = solv_dupjoin("0-", f->matches[mi].str, NULL);

        for (Id id = 1; id < pool->nsolvables; ++id) {
            if (!MAPTST(q->result, id))
                continue;
            char *e, *v, *r;
            Solvable *s = pool_id2solvable(pool, id);
            if (s->evr == ID_EMPTY)
                continue;
            const char *evr = pool_id2str(pool, s->evr);

            pool_split_evr(pool, evr, &e, &v, &r);

            if (cmp_type == HY_GLOB) {
                if (fnmatch(match, r, 0))
                    continue;
                MAPSET(m, id);
            }

            char *vr = pool_tmpjoin(pool, "0-", r, NULL);

            int cmp = pool_evrcmp_str(pool, vr, filter_vr, EVRCMP_COMPARE);

            if ((cmp > 0 && f->cmp_type & HY_GT) ||
                (cmp < 0 && f->cmp_type & HY_LT) ||
                (cmp == 0 && f->cmp_type & HY_EQ))
                MAPSET(m, id);
        }
        g_free(filter_vr);
    }
}

static void
filter_sourcerpm(HyQuery q, struct _Filter *f, Map *m)
{
    Pool *pool = dnf_sack_get_pool(q->sack);

    for (int mi = 0; mi < f->nmatches; ++mi) {
        const char *match = f->matches[mi].str;

        for (Id id = 1; id < pool->nsolvables; ++id) {
            if (!MAPTST(q->result, id))
                continue;
            Solvable *s = pool_id2solvable(pool, id);

            const char *name = solvable_lookup_str(s, SOLVABLE_SOURCENAME);
            if (name == NULL)
                name = pool_id2str(pool, s->name);
            if (!g_str_has_prefix(match, name)) // early check
                continue;

            DnfPackage *pkg = dnf_package_new(q->sack, id);
            const char *srcrpm = dnf_package_get_sourcerpm(pkg);
            if (srcrpm && !strcmp(match, srcrpm))
                MAPSET(m, id);
            g_object_unref(pkg);
        }
    }
}

static void
filter_obsoletes(HyQuery q, struct _Filter *f, Map *m)
{
    Pool *pool = dnf_sack_get_pool(q->sack);
    int obsprovides = pool_get_flag(pool, POOL_FLAG_OBSOLETEUSESPROVIDES);
    Map *target;

    assert(f->match_type == _HY_PKG);
    assert(f->nmatches == 1);
    target = dnf_packageset_get_map(f->matches[0].pset);
    dnf_sack_make_provides_ready(q->sack);
    for (Id p = 1; p < pool->nsolvables; ++p) {
        if (!MAPTST(q->result, p))
            continue;
        Solvable *s = pool_id2solvable(pool, p);
        if (!s->repo)
            continue;
        for (Id *r_id = s->repo->idarraydata + s->obsoletes; *r_id; ++r_id) {
            Id r, rr;

            FOR_PROVIDES(r, rr, *r_id) {
                if (!MAPTST(target, r))
                    continue;
                assert(r != SYSTEMSOLVABLE);
                Solvable *so = pool_id2solvable(pool, r);
                if (!obsprovides && !pool_match_nevr(pool, so, *r_id))
                    continue; /* only matching pkg names */
                MAPSET(m, p);
                break;
            }
        }
    }
}

static void
filter_provides_reldep(HyQuery q, struct _Filter *f, Map *m)
{
    Pool *pool = dnf_sack_get_pool(q->sack);
    Id p, pp;

    dnf_sack_make_provides_ready(q->sack);
    for (int i = 0; i < f->nmatches; ++i) {
        Id r_id = dnf_reldep_get_id (f->matches[i].reldep);
        FOR_PROVIDES(p, pp, r_id)
            MAPSET(m, p);
    }
}

static void
filter_rco_reldep(HyQuery q, struct _Filter *f, Map *m)
{
    assert(f->match_type == _HY_RELDEP);

    Pool *pool = dnf_sack_get_pool(q->sack);
    Id rco_key = reldep_keyname2id(f->keyname);
    Queue rco;

    queue_init(&rco);
    for (int i = 0; i < f->nmatches; ++i) {
        Id r_id = dnf_reldep_get_id (f->matches[i].reldep);

        for (Id s_id = 1; s_id < pool->nsolvables; ++s_id) {
            if (!MAPTST(q->result, s_id))
                continue;

            Solvable *s = pool_id2solvable(pool, s_id);

            queue_empty(&rco);
            solvable_lookup_idarray(s, rco_key, &rco);
            for (int j = 0; j < rco.count; ++j) {
                Id r_id2 = rco.elements[j];

                if (pool_match_dep(pool, r_id, r_id2)) {
                    MAPSET(m, s_id);
                    break;
                }
            }
        }
    }
    queue_free(&rco);
}

static void
filter_reponame(HyQuery q, struct _Filter *f, Map *m)
{
    Pool *pool = dnf_sack_get_pool(q->sack);
    int i;
    Solvable *s;
    Repo *r;
    Id id, ourids[pool->nrepos];

    for (id = 0; id < pool->nrepos; ++id)
        ourids[id] = 0;
    FOR_REPOS(id, r) {
        for (i = 0; i < f->nmatches; i++) {
            if (!strcmp(r->name, f->matches[i].str)) {
                ourids[id] = 1;
                break;
            }
        }
    }

    for (i = 1; i < pool->nsolvables; ++i) {
        if (!MAPTST(q->result, i))
            continue;
        s = pool_id2solvable(pool, i);
        switch (f->cmp_type & ~HY_COMPARISON_FLAG_MASK) {
        case HY_EQ:
            if (s->repo && ourids[s->repo->repoid])
                MAPSET(m, i);
            break;
        default:
            assert(0);
        }
    }
}

static void
filter_location(HyQuery q, struct _Filter *f, Map *m)
{
    Pool *pool = dnf_sack_get_pool(q->sack);

    for (int mi = 0; mi < f->nmatches; ++mi) {
        const char *match = f->matches[mi].str;

        for (Id id = 1; id < pool->nsolvables; ++id) {
            if (!MAPTST(q->result, id))
                continue;
            Solvable *s = pool_id2solvable(pool, id);

            const char *location = solvable_get_location(s, NULL);
            if (location == NULL)
                continue;
            if (!strcmp(match, location))
                MAPSET(m, id);
        }
    }
}

static void
filter_nevra(HyQuery q, struct _Filter *f, Map *m)
{
    Pool *pool = dnf_sack_get_pool(q->sack);
    int fn_flags = (HY_ICASE & f->cmp_type) ? FNM_CASEFOLD : 0;

    for (Id id = 1; id < pool->nsolvables; ++id) {
        if (!MAPTST(q->result, id))
            continue;
        Solvable* s = pool_id2solvable(pool, id);
        const char* nevra = pool_solvable2str(pool, s);

        for (int mi = 0; mi < f->nmatches; ++mi) {
             const char *nevra_pattern = f->matches[mi].str;
             if (!(HY_GLOB & f->cmp_type)) {
                 if (strcmp(nevra_pattern, nevra) == 0)
                     MAPSET(m, id);
             } else if (fnmatch(nevra_pattern, nevra, fn_flags) == 0) {
                 MAPSET(m, id);
             }
        }
    }
}

static void
filter_updown(HyQuery q, int downgrade, Map *res)
{
    DnfSack *sack = q->sack;
    Pool *pool = dnf_sack_get_pool(sack);
    int i;
    Map m;

    assert(pool->installed);
    dnf_sack_make_provides_ready(q->sack);
    map_init(&m, pool->nsolvables);
    for (i = 1; i < pool->nsolvables; ++i) {
        if (!MAPTST(res, i))
            continue;
        Solvable *s = pool_id2solvable(pool, i);
        if (s->repo == pool->installed)
            continue;
        if (downgrade && what_downgrades(pool, i) > 0)
            MAPSET(&m, i);
        else if (!downgrade && what_upgrades(pool, i) > 0)
            MAPSET(&m, i);
    }

    map_and(res, &m);
    map_free(&m);
}

static void
filter_updown_able(HyQuery q, int downgradable, Map *res)
{
    Id p, what;
    Solvable *s;
    Map m;
    Pool *pool = dnf_sack_get_pool(q->sack);

    assert(pool->installed);
    dnf_sack_make_provides_ready(q->sack);
    map_init(&m, pool->nsolvables);
    FOR_PKG_SOLVABLES(p) {
        s = pool_id2solvable(pool, p);
        if (s->repo == pool->installed)
            continue;

        what = downgradable ? what_downgrades(pool, p) :
                              what_upgrades(pool, p);
        if (what != 0 && map_tst(res, what))
            map_set(&m, what);
    }

    map_and(res, &m);
    map_free(&m);
}

static int
filter_latest_sortcmp(const void *ap, const void *bp, void *dp)
{
    Pool *pool = dp;
    Solvable *sa = pool->solvables + *(Id *)ap;
    Solvable *sb = pool->solvables + *(Id *)ap;
    int r;
    r = sa->name - sb->name;
    if (r)
        return r;
    return *(Id *)ap - *(Id *)bp;
}

static int
filter_latest_sortcmp_byarch(const void *ap, const void *bp, void *dp)
{
    Pool *pool = dp;
    Solvable *sa = pool->solvables + *(Id *)ap;
    Solvable *sb = pool->solvables + *(Id *)bp;
    int r;
    r = sa->name - sb->name;
    if (r)
        return r;
    r = sa->arch - sb->arch;
    if (r)
        return r;
    return *(Id *)ap - *(Id *)bp;
}

static void
filter_latest(HyQuery q, Map *res)
{
    Pool *pool = dnf_sack_get_pool(q->sack);
    Queue samename;

    queue_init(&samename);
    for (int i = 1; i < pool->nsolvables; ++i)
        if (MAPTST(res, i))
            queue_push(&samename, i);

    if (samename.count < 2) {
        queue_free(&samename);
        return;
    }

    if (q->latest_per_arch)
        solv_sort(samename.elements, samename.count, sizeof(Id),
                  filter_latest_sortcmp_byarch, pool);
    else
        solv_sort(samename.elements, samename.count, sizeof(Id),
                  filter_latest_sortcmp, pool);

    Solvable *considered, *highest = 0;
    Id hp = 0;

    for (int i = 0; i < samename.count; ++i) {
        Id p = samename.elements[i];
        considered = pool->solvables + p;
        if (!highest || highest->name != considered->name ||
            (q->latest_per_arch && highest->arch != considered->arch)) {
            /* start of a new block */
            hp = p;
            highest = considered;
            continue;
        }
        if (pool_evrcmp(pool,highest->evr, considered->evr, EVRCMP_COMPARE) < 0) {
            /* new highest found */
            MAPCLR(res, hp);
            hp = p;
            highest = considered;
        } else {
            /* note this is taken also for the same version case */
            MAPCLR(res, p);
        }
    }
    queue_free(&samename);
}

static void
filter_advisory(HyQuery q, struct _Filter *f, Map *m, int keyname)
{
    Pool *pool = dnf_sack_get_pool(q->sack);
    DnfAdvisory *advisory;
    g_autoptr(GPtrArray) pkgs = g_ptr_array_new_with_free_func((GDestroyNotify) g_object_unref);
    Dataiterator di;
    gboolean eq;

    // iterate over advisories
    dataiterator_init(&di, pool, 0, 0, 0, 0, 0);
    dataiterator_prepend_keyname(&di, UPDATE_COLLECTION);
    while (dataiterator_step(&di)) {
        dataiterator_setpos_parent(&di);
        advisory = dnf_advisory_new(pool, di.solvid);

        for (int mi = 0; mi < f->nmatches; ++mi) {
            const char *match = f->matches[mi].str;
            switch(keyname) {
            case HY_PKG_ADVISORY:
                eq = dnf_advisory_match_id(advisory, match);
                break;
            case HY_PKG_ADVISORY_BUG:
                eq = dnf_advisory_match_bug(advisory, match);
                break;
            case HY_PKG_ADVISORY_CVE:
                eq = dnf_advisory_match_cve(advisory, match);
                break;
            case HY_PKG_ADVISORY_TYPE:
                eq = dnf_advisory_match_kind(advisory, match);
                break;
            case HY_PKG_ADVISORY_SEVERITY:
                eq = dnf_advisory_match_severity(advisory, match);
                break;
            default:
                eq = FALSE;
            }
            if (eq) {
                // remember package nevras for matched advisories
                GPtrArray *apkgs = dnf_advisory_get_packages(advisory);
                DnfAdvisoryPkg *apkg;
                for (unsigned int p = 0; p < apkgs->len; ++p) {
                     apkg = g_ptr_array_index(apkgs, p);
                     g_ptr_array_add(pkgs, g_object_ref(apkg));
                }
                g_ptr_array_unref(apkgs);
            }
        }
        g_object_unref(advisory);
        dataiterator_skip_solvable(&di);
    }

    // convert nevras (from DnfAdvisoryPkg) to pool ids
    for (Id id = 1; id < pool->nsolvables; ++id) {
        if (pkgs->len == 0)
            break;
        if (!MAPTST(q->result, id))
            continue;
        Solvable* s = pool_id2solvable(pool, id);
        for (unsigned int p = 0; p < pkgs->len; ++p) {
            DnfAdvisoryPkg *apkg = g_ptr_array_index(pkgs, p);
            if (dnf_advisorypkg_compare_solvable(apkg, pool, s) == 0) {
                MAPSET(m, id);
                // found it, now remove it from the list to speed up rest of query
                g_ptr_array_remove_index(pkgs, p);
                break;
            }
        }
    }
    dataiterator_free(&di);
}

static void
clear_filters(HyQuery q)
{
    for (int i = 0; i < q->nfilters; ++i) {
        struct _Filter *filterp = q->filters + i;
        filter_reinit(filterp, 0);
    }
    g_free(q->filters);
    q->filters = NULL;
    q->nfilters = 0;
    q->downgradable = 0;
    q->downgrades = 0;
    q->updatable = 0;
    q->updates = 0;
    q->latest = 0;
    q->latest_per_arch = 0;
}

static void
init_result(HyQuery q)
{
    Pool *pool = dnf_sack_get_pool(q->sack);
    Id solvid;

    q->result = g_malloc0(sizeof(Map));
    map_init(q->result, pool->nsolvables);
    FOR_PKG_SOLVABLES(solvid)
        map_set(q->result, solvid);
    if (!(q->flags & HY_IGNORE_EXCLUDES)) {
        dnf_sack_recompute_considered(q->sack);
        if (pool->considered)
            map_and(q->result, pool->considered);
    }

    // make sure the odd bits are cleared:
    unsigned total_bits = q->result->size << 3;
    for (guint i = pool->nsolvables; i < total_bits; ++i)
        MAPCLR(q->result, i);
}

void
hy_query_apply(HyQuery q)
{
    Pool *pool = dnf_sack_get_pool(q->sack);
    Map m;

    if (q->applied)
        return;
    if (!q->result)
        init_result(q);
    map_init(&m, pool->nsolvables);
    assert(m.size == q->result->size);
    for (int i = 0; i < q->nfilters; ++i) {
        struct _Filter *f = q->filters + i;

        map_empty(&m);
        switch (f->keyname) {
        case HY_PKG:
            filter_pkg(q, f, &m);
            break;
        case HY_PKG_ALL:
            filter_all(q, f, &m);
            break;
        case HY_PKG_CONFLICTS:
            filter_rco_reldep(q, f, &m);
            break;
        case HY_PKG_EPOCH:
            filter_epoch(q, f, &m);
            break;
        case HY_PKG_EVR:
            filter_evr(q, f, &m);
            break;
        case HY_PKG_NEVRA:
            filter_nevra(q, f, &m);
            break;
        case HY_PKG_VERSION:
            filter_version(q, f, &m);
            break;
        case HY_PKG_RELEASE:
            filter_release(q, f, &m);
            break;
        case HY_PKG_SOURCERPM:
            filter_sourcerpm(q, f, &m);
            break;
        case HY_PKG_OBSOLETES:
            if (f->match_type == _HY_RELDEP)
                filter_rco_reldep(q, f, &m);
            else {
                assert(f->match_type == _HY_PKG);
                filter_obsoletes(q, f, &m);
            }
            break;
        case HY_PKG_PROVIDES:
            assert(f->match_type == _HY_RELDEP);
            filter_provides_reldep(q, f, &m);
            break;
        case HY_PKG_ENHANCES:
        case HY_PKG_RECOMMENDS:
        case HY_PKG_REQUIRES:
        case HY_PKG_SUGGESTS:
        case HY_PKG_SUPPLEMENTS:
            assert(f->match_type == _HY_RELDEP);
            filter_rco_reldep(q, f, &m);
            break;
        case HY_PKG_REPONAME:
            filter_reponame(q, f, &m);
            break;
        case HY_PKG_LOCATION:
            filter_location(q, f, &m);
            break;
        case HY_PKG_ADVISORY:
        case HY_PKG_ADVISORY_BUG:
        case HY_PKG_ADVISORY_CVE:
        case HY_PKG_ADVISORY_SEVERITY:
        case HY_PKG_ADVISORY_TYPE:
            filter_advisory(q, f, &m, f->keyname);
            break;
        default:
            filter_dataiterator(q, f, &m);
        }
        if (f->cmp_type & HY_NOT)
            map_subtract(q->result, &m);
        else
            map_and(q->result, &m);
    }
    map_free(&m);
    if (q->downgradable)
        filter_updown_able(q, 1, q->result);
    if (q->downgrades)
        filter_updown(q, 1, q->result);
    if (q->updatable)
        filter_updown_able(q, 0, q->result);
    if (q->updates)
        filter_updown(q, 0, q->result);
    if (q->latest)
        filter_latest(q, q->result);

    q->applied = 1;
    clear_filters(q);
}

HyQuery
hy_query_create(DnfSack *sack)
{
    return hy_query_create_flags(sack, 0);
}

HyQuery
hy_query_create_flags(DnfSack *sack, int flags)
{
    HyQuery q = g_malloc0(sizeof(*q));
    q->sack = sack;
    q->flags = flags;
    return q;
}

void
hy_query_free(HyQuery q)
{
    hy_query_clear(q);
    g_free(q);
}

void
hy_query_clear(HyQuery q)
{
    if (q->result) {
        map_free(q->result);
        g_free(q->result);
        q->result = NULL;
    }
    clear_filters(q);
}

HyQuery
hy_query_clone(HyQuery q)
{
    HyQuery qn = hy_query_create(q->sack);

    qn->flags = q->flags;
    qn->downgradable = q->downgradable;
    qn->downgrades = q->downgrades;
    qn->updatable = q->updatable;
    qn->updates = q->updates;
    qn->latest = q->latest;
    qn->latest_per_arch = q->latest_per_arch;
    qn->applied = q->applied;

    for (int i = 0; i < q->nfilters; ++i) {
        struct _Filter *filterp = query_add_filter(qn, q->filters[i].nmatches);

        filterp->cmp_type = q->filters[i].cmp_type;
        filterp->keyname = q->filters[i].keyname;
        filterp->match_type = q->filters[i].match_type;
        for (int j = 0; j < q->filters[i].nmatches; ++j) {
            char *str_copy;
            DnfPackageSet *pset;
            DnfReldep *reldep;

            switch (filterp->match_type) {
            case _HY_NUM:
                filterp->matches[j].num = q->filters[i].matches[j].num;
                break;
            case _HY_PKG:
                pset = q->filters[i].matches[j].pset;
                filterp->matches[j].pset = dnf_packageset_clone(pset);
                break;
            case _HY_RELDEP:
                reldep = q->filters[i].matches[j].reldep;
                filterp->matches[j].reldep = g_object_ref (reldep);
                break;
            case _HY_STR:
                str_copy = g_strdup(q->filters[i].matches[j].str);
                filterp->matches[j].str = str_copy;
                break;
            default:
                assert(0);
            }
        }
    }
    assert(qn->nfilters == q->nfilters);
    if (q->result) {
        qn->result = g_malloc0(sizeof(Map));
        map_init_clone(qn->result, q->result);
    }

    return qn;
}

int
hy_query_filter(HyQuery q, int keyname, int cmp_type, const char *match)
{
    if (!valid_filter_str(keyname, cmp_type))
        return DNF_ERROR_BAD_QUERY;
    q->applied = 0;

    switch (keyname) {
    case HY_PKG_CONFLICTS:
    case HY_PKG_ENHANCES:
    case HY_PKG_OBSOLETES:
    case HY_PKG_PROVIDES:
    case HY_PKG_RECOMMENDS:
    case HY_PKG_REQUIRES:
    case HY_PKG_SUGGESTS:
    case HY_PKG_SUPPLEMENTS: {
        DnfSack *sack = query_sack(q);

    if (cmp_type == HY_GLOB) {
        DnfReldepList *reldeplist = reldeplist_from_str (sack, match);
        hy_query_filter_reldep_in(q, keyname, reldeplist);
        g_object_unref (reldeplist);
        return 0;
    } else {
        DnfReldep *reldep = reldep_from_str (sack, match);
        if (reldep == NULL)
            return hy_query_filter_empty(q);
        int ret = hy_query_filter_reldep(q, keyname, reldep);
        g_object_unref (reldep);
        return ret;
    }
    }
    default: {
        struct _Filter *filterp = query_add_filter(q, 1);
        filterp->cmp_type = cmp_type;
        filterp->keyname = keyname;
        filterp->match_type = _HY_STR;
        filterp->matches[0].str = g_strdup(match);
        return 0;
    }
    }
}

int
hy_query_filter_empty(HyQuery q)
{
    struct _Filter *filterp = query_add_filter(q, 1);
    filterp->cmp_type = HY_EQ;
    filterp->keyname = HY_PKG_ALL;
    filterp->match_type = _HY_NUM;
    filterp->matches[0].num = -1;
    return 0;
}

int
hy_query_filter_in(HyQuery q, int keyname, int cmp_type,
                   const char **matches)
{
    if (!valid_filter_str(keyname, cmp_type))
        return DNF_ERROR_BAD_QUERY;
    q->applied = 0;

    const unsigned count = g_strv_length((gchar**)matches);
    struct _Filter *filterp = query_add_filter(q, count);

    filterp->cmp_type = cmp_type;
    filterp->keyname = keyname;
    filterp->match_type = _HY_STR;
    for (guint i = 0; i < count; ++i)
        filterp->matches[i].str = g_strdup(matches[i]);
    return 0;
}

int
hy_query_filter_num(HyQuery q, int keyname, int cmp_type, int match)
{
    if (!valid_filter_num(keyname, cmp_type))
        return DNF_ERROR_BAD_QUERY;
    q->applied = 0;

    struct _Filter *filterp = query_add_filter(q, 1);
    filterp->cmp_type = cmp_type;
    filterp->keyname = keyname;
    filterp->match_type = _HY_NUM;
    filterp->matches[0].num = match;
    return 0;
}

int
hy_query_filter_num_in(HyQuery q, int keyname, int cmp_type, int nmatches,
                       const int *matches)
{
    if (!valid_filter_num(keyname, cmp_type))
        return DNF_ERROR_BAD_QUERY;
    q->applied = 0;

    struct _Filter *filterp = query_add_filter(q, nmatches);

    filterp->cmp_type = cmp_type;
    filterp->keyname = keyname;
    filterp->match_type = _HY_NUM;
    for (int i = 0; i < nmatches; ++i)
        filterp->matches[i].num = matches[i];
    return 0;
}

int
hy_query_filter_package_in(HyQuery q, int keyname, int cmp_type,
                           DnfPackageSet *pset)
{
    if (!valid_filter_pkg(keyname, cmp_type))
        return DNF_ERROR_BAD_QUERY;
    q->applied = 0;

    struct _Filter *filterp = query_add_filter(q, 1);
    filterp->cmp_type = cmp_type;
    filterp->keyname = keyname;
    filterp->match_type = _HY_PKG;
    filterp->matches[0].pset = dnf_packageset_clone(pset);
    return 0;
}

int
hy_query_filter_reldep(HyQuery q, int keyname, DnfReldep *reldep)
{
    if (!valid_filter_reldep(keyname))
        return DNF_ERROR_BAD_QUERY;
    q->applied = 0;

    struct _Filter *filterp = query_add_filter(q, 1);
    filterp->cmp_type = HY_EQ;
    filterp->keyname = keyname;
    filterp->match_type = _HY_RELDEP;
    filterp->matches[0].reldep = g_object_ref (reldep);
    return 0;
}

int
hy_query_filter_reldep_in(HyQuery q, int keyname, DnfReldepList *reldeplist)
{
    if (!valid_filter_reldep(keyname))
        return DNF_ERROR_BAD_QUERY;
    q->applied = 0;

    const int nmatches = dnf_reldep_list_count (reldeplist);
    struct _Filter *filterp = query_add_filter(q, nmatches);
    filterp->cmp_type = HY_EQ;
    filterp->keyname = keyname;
    filterp->match_type = _HY_RELDEP;

    for (int i = 0; i < nmatches; ++i)
        filterp->matches[i].reldep = dnf_reldep_list_index (reldeplist, i);
    return 0;
}

int
hy_query_filter_provides(HyQuery q, int cmp_type, const char *name,
                         const char *evr)
{
    DnfReldep *reldep = dnf_reldep_new (query_sack(q), name, cmp_type, evr);
    assert(reldep);
    int ret = hy_query_filter_reldep(q, HY_PKG_PROVIDES, reldep);
    g_object_unref (reldep);
    return ret;
}

int
hy_query_filter_provides_in(HyQuery q, char **reldep_strs)
{
    int cmp_type;
    char *name = NULL;
    char *evr = NULL;
    DnfReldep *reldep;
    DnfReldepList *reldeplist = dnf_reldep_list_new (q->sack);
    for (int i = 0; reldep_strs[i] != NULL; ++i) {
        if (parse_reldep_str(reldep_strs[i], &name, &evr, &cmp_type) == -1) {
            g_object_unref(reldeplist);
            return DNF_ERROR_BAD_QUERY;
        }
        reldep = dnf_reldep_new (q->sack, name, cmp_type, evr);
        if (reldep) {
            dnf_reldep_list_add (reldeplist, reldep);
            g_object_unref (reldep);
        }
        g_free(name);
        g_free(evr);
    }
    hy_query_filter_reldep_in(q, HY_PKG_PROVIDES, reldeplist);
    g_object_unref (reldeplist);
    return 0;
}

int
hy_query_filter_requires(HyQuery q, int cmp_type, const char *name, const char *evr)
{
    /* convert to a reldep filter. the trick is handling negation right (it gets
    resolved in hy_query_apply(), we just have to make sure to store it in the
    filter. */
    int not_neg = cmp_type & ~HY_NOT;
    DnfReldep *reldep = dnf_reldep_new (q->sack, name, not_neg, evr);
    int rc;
    if (reldep) {
        rc = hy_query_filter_reldep(q, HY_PKG_REQUIRES, reldep);
        g_object_unref (reldep);
        q->filters[q->nfilters - 1].cmp_type = cmp_type;
    } else
        rc = hy_query_filter_empty(q);
    return rc;
}

/**
 * Narrows to only those installed packages for which there is a downgrading package.
 */
void
hy_query_filter_downgradable(HyQuery q, int val)
{
    q->applied = 0;
    q->downgradable = val;
}

/**
 * Narrows to only packages downgrading installed packages.
 */
void
hy_query_filter_downgrades(HyQuery q, int val)
{
    q->applied = 0;
    q->downgrades = val;
}

/**
 * Narrows to only those installed packages for which there is an updating package.
 */
void
hy_query_filter_upgradable(HyQuery q, int val)
{
    q->applied = 0;
    q->updatable = val;
}

/**
 * Narrows to only packages updating installed packages.
 */
void
hy_query_filter_upgrades(HyQuery q, int val)
{
    q->applied = 0;
    q->updates = val;
}

/**
 * Narrows to only the highest version of a package per arch.
 */
void
hy_query_filter_latest_per_arch(HyQuery q, int val)
{
    q->applied = 0;
    q->latest_per_arch = 1;
    q->latest = val;
}

/**
 * Narrows to only the highest version of a package.
 */
void
hy_query_filter_latest(HyQuery q, int val)
{
    q->applied = 0;
    q->latest_per_arch = 0;
    q->latest = val;
}

GPtrArray *
hy_query_run(HyQuery q)
{
    Pool *pool = dnf_sack_get_pool(q->sack);
    GPtrArray *plist = hy_packagelist_create();

    hy_query_apply(q);
    for (int i = 1; i < pool->nsolvables; ++i)
        if (MAPTST(q->result, i))
            g_ptr_array_add(plist, dnf_package_new(q->sack, i));
    return plist;
}

DnfPackageSet *
hy_query_run_set(HyQuery q)
{
    hy_query_apply(q);
    return dnf_packageset_from_bitmap(q->sack, q->result);
}

/**
 * hy_query_union:
 * @q:     a #HyQuery instance
 * @other: other #HyQuery instance
 *
 * Unites query with other query (aka logical or).
 *
 * Returns: Nothing.
 *
 * Since: 0.7.0
 */
void
hy_query_union(HyQuery q, HyQuery other)
{
    hy_query_apply(q);
    hy_query_apply(other);
    map_or(q->result, other->result);
}

/**
 * hy_query_intersection:
 * @q:     a #HyQuery instance
 * @other: other #HyQuery instance
 *
 * Intersects query with other query (aka logical and).
 *
 * Returns: Nothing.
 *
 * Since: 0.7.0
 */
void
hy_query_intersection(HyQuery q, HyQuery other)
{
    hy_query_apply(q);
    hy_query_apply(other);
    map_and(q->result, other->result);
}

/**
 * hy_query_difference:
 * @q:     a #HyQuery instance
 * @other: other #HyQuery instance
 *
 * Computes difference between query and other query (aka q and not other).
 *
 * Returns: Nothing.
 *
 * Since: 0.7.0
 */
void
hy_query_difference(HyQuery q, HyQuery other)
{
    hy_query_apply(q);
    hy_query_apply(other);
    map_subtract(q->result, other->result);
}
