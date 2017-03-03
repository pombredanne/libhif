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

#include <wordexp.h>


#include <solv/pool.h>
#include <solv/repo.h>
#include <solv/testcase.h>


#include "libdnf/hy-repo.h"
#include "libdnf/hy-repo-private.h"
#include "testshared.h"

HyRepo
glob_for_repofiles(Pool *pool, const char *repo_name, const char *path)
{
    HyRepo repo = hy_repo_create(repo_name);
    const char *tmpl;
    wordexp_t word_vector;

    tmpl = pool_tmpjoin(pool, path, "/repomd.xml", NULL);
    if (wordexp(tmpl, &word_vector, 0) || word_vector.we_wordc < 1)
        goto fail;
    hy_repo_set_string(repo, HY_REPO_MD_FN, word_vector.we_wordv[0]);

    tmpl = pool_tmpjoin(pool, path, "/*primary.xml.gz", NULL);
    if (wordexp(tmpl, &word_vector, WRDE_REUSE) || word_vector.we_wordc < 1)
        goto fail;
    hy_repo_set_string(repo, HY_REPO_PRIMARY_FN, word_vector.we_wordv[0]);

    tmpl = pool_tmpjoin(pool, path, "/*filelists.xml.gz", NULL);
    if (wordexp(tmpl, &word_vector, WRDE_REUSE) || word_vector.we_wordc < 1)
        goto fail;
    hy_repo_set_string(repo, HY_REPO_FILELISTS_FN, word_vector.we_wordv[0]);

    tmpl = pool_tmpjoin(pool, path, "/*prestodelta.xml.gz", NULL);
    if (wordexp(tmpl, &word_vector, WRDE_REUSE) || word_vector.we_wordc < 1)
        goto fail;
    hy_repo_set_string(repo, HY_REPO_PRESTO_FN, word_vector.we_wordv[0]);

    tmpl = pool_tmpjoin(pool, path, "/*updateinfo.xml.gz", NULL);
    if (wordexp(tmpl, &word_vector, WRDE_REUSE) || word_vector.we_wordc < 1)
        goto fail;
    hy_repo_set_string(repo, HY_REPO_UPDATEINFO_FN, word_vector.we_wordv[0]);

    wordfree(&word_vector);
    return repo;

 fail:
    wordfree(&word_vector);
    hy_repo_free(repo);
    return NULL;
}

int
load_repo(Pool *pool, const char *name, const char *path, int installed)
{
    HyRepo hrepo = hy_repo_create(name);
    Repo *r = repo_create(pool, name);
    hrepo->libsolv_repo = r;
    r->appdata = hrepo;

    FILE *fp = fopen(path, "r");

    if (!fp)
        return 1;
    testcase_add_testtags(r,  fp, 0);
    if (installed)
        pool_set_installed(pool, r);
    fclose(fp);
    return 0;
}
