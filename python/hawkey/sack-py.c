/*
 * Copyright (C) 2012-2015 Red Hat, Inc.
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

#include "Python.h"

// hawkey
#include "hif-types.h"
#include "hy-package-private.h"
#include "hy-packageset.h"
#include "hy-repo.h"
#include "hif-sack-private.h"
#include "hy-util.h"
#include "hif-version.h"

// pyhawkey
#include "exception-py.h"
#include "hawkey-pysys.h"
#include "iutil-py.h"
#include "package-py.h"
#include "repo-py.h"
#include "sack-py.h"

#include "pycomp.h"

typedef struct {
    PyObject_HEAD
    HifSack *sack;
    PyObject *custom_package_class;
    PyObject *custom_package_val;
    FILE *log_out;
} _SackObject;

PyObject *
new_package(PyObject *sack, Id id)
{
    _SackObject *self;

    if (!sackObject_Check(sack)) {
        PyErr_SetString(PyExc_TypeError, "Expected a _hawkey.Sack object.");
        return NULL;
    }
    self = (_SackObject*)sack;
    PyObject *arglist;
    if (self->custom_package_class || self->custom_package_val) {
        arglist = Py_BuildValue("(Oi)O", sack, id, self->custom_package_val);
    } else {
        arglist = Py_BuildValue("((Oi))", sack, id);
    }
    if (arglist == NULL)
        return NULL;
    PyObject *package;
    if (self->custom_package_class) {
        package = PyObject_CallObject(self->custom_package_class, arglist);
    } else {
        package = PyObject_CallObject((PyObject*)&package_Type, arglist);
    }
    Py_DECREF(arglist);
    return package;
}

HifSack *
sackFromPyObject(PyObject *o)
{
    if (!sackObject_Check(o)) {
        PyErr_SetString(PyExc_TypeError, "Expected a _hawkey.Sack object.");
        return NULL;
    }
    return ((_SackObject *)o)->sack;
}

int
sack_converter(PyObject *o, HifSack **sack_ptr)
{
    HifSack *sack = sackFromPyObject(o);
    if (sack == NULL)
        return 0;
    *sack_ptr = sack;
    return 1;
}

/* helpers */
static PyObject *
repo_enabled(_SackObject *self, PyObject *reponame, int enabled)
{
    PyObject *tmp_py_str = NULL;
    const char *cname = pycomp_get_string(reponame, &tmp_py_str);

    if (cname == NULL) {
        Py_XDECREF(tmp_py_str);
        return NULL;
    }
    hif_sack_repo_enabled(self->sack, cname, enabled);
    Py_XDECREF(tmp_py_str);
    Py_RETURN_NONE;
}

/* functions on the type */

static void
sack_dealloc(_SackObject *o)
{
    if (o->sack)
        g_object_unref(o->sack);
    if (o->log_out)
        fclose(o->log_out);
    Py_TYPE(o)->tp_free(o);
}

static PyObject *
sack_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    _SackObject *self = (_SackObject *)type->tp_alloc(type, 0);

    if (self) {
        self->sack = NULL;
        self->custom_package_class = NULL;
        self->custom_package_val = NULL;
    }
    return (PyObject *)self;
}

const char *
log_level_name(int level)
{
    switch (level) {
    case G_LOG_FLAG_FATAL:
        return "FATAL";
    case G_LOG_LEVEL_ERROR:
        return "ERROR";
    case G_LOG_LEVEL_CRITICAL:
        return "CRITICAL";
    case G_LOG_LEVEL_WARNING:
        return "WARN";
    case G_LOG_LEVEL_DEBUG:
        return "DEBUG";
    case G_LOG_LEVEL_INFO:
        return "INFO";
    default:
        return "(level?)";
    }
}

static void
log_handler(const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer user_data)
{
    time_t t = time(NULL);
    struct tm tm;
    char timestr[26];

    FILE *log_out = (FILE*) user_data;
    localtime_r(&t, &tm);
    strftime(timestr, 26, "%b-%d %H:%M:%S ", &tm);
    gchar *msg = g_strjoin("", log_level_name(log_level), " ",
                           timestr, message, "\n", NULL);
    fwrite(msg, strlen(msg), 1, log_out);
    fflush(log_out);
    g_free(msg);
}

gboolean
set_logfile(const gchar *path, FILE *log_out)
{
    log_out = fopen(path, "a");

    if (!log_out)
        return FALSE;

    g_log_set_default_handler(log_handler, log_out);
    g_info("=== Started libhif-%d.%d.%d ===", HIF_MAJOR_VERSION,
            HIF_MINOR_VERSION, HIF_MICRO_VERSION);
    return TRUE;
}

static int
sack_init(_SackObject *self, PyObject *args, PyObject *kwds)
{
    g_autoptr(GError) error = NULL;
    PyObject *custom_class = NULL;
    PyObject *custom_val = NULL;
    const char *cachedir = NULL;
    const char *arch = NULL;
    const char *rootdir = NULL;
    const char *logfile = NULL;
    PyObject *tmp_py_str = NULL;
    PyObject *tmp2_py_str = NULL;
    PyObject *cachedir_py = NULL;
    PyObject *logfile_py = NULL;
    self->log_out = NULL;
    int make_cache_dir = 0;
    const char *kwlist[] = {"cachedir", "arch", "rootdir", "pkgcls",
                      "pkginitval", "make_cache_dir", "logfile", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|OssOOiO", (char**) kwlist,
                                     &cachedir_py, &arch, &rootdir,
                                     &custom_class, &custom_val,
                                     &make_cache_dir, &logfile_py))
        return -1;
    if (cachedir_py != NULL)
        cachedir = pycomp_get_string(cachedir_py, &tmp_py_str);
    int flags = 0;
    if (make_cache_dir)
        flags |= HIF_SACK_SETUP_FLAG_MAKE_CACHE_DIR;
    self->sack = hif_sack_new();
    if (!hif_sack_set_arch(self->sack, arch, &error)) {
        PyErr_SetString(HyExc_Arch, "Unrecognized arch for the sack.");
        return -1;
    }
    hif_sack_set_rootdir(self->sack, rootdir);
    hif_sack_set_cachedir(self->sack, cachedir);
    if (logfile_py != NULL) {
        logfile = pycomp_get_string(logfile_py, &tmp2_py_str);
        if (!set_logfile(logfile, self->log_out)) {
            PyErr_Format(PyExc_IOError, "Failed to open log file: %s", logfile);
            return -1;
        }
    }
    Py_XDECREF(tmp_py_str);
    Py_XDECREF(tmp2_py_str);
    if (!hif_sack_setup(self->sack, flags, &error)) {
        switch (error->code) {
        case HIF_ERROR_FILE_INVALID:
            PyErr_SetString(PyExc_IOError,
                            "Failed creating working files for the Sack.");
            break;
        case HIF_ERROR_INVALID_ARCHITECTURE:
            PyErr_SetString(HyExc_Arch, "Unrecognized arch for the sack.");
            break;
        default:
            assert(0);
        }
        return -1;
    }

    if (custom_class && custom_class != Py_None) {
        if (!PyType_Check(custom_class)) {
            PyErr_SetString(PyExc_TypeError, "Expected a class object.");
            return -1;
        }
        Py_INCREF(custom_class);
        self->custom_package_class = custom_class;
    }
    if (custom_val && custom_val != Py_None) {
        Py_INCREF(custom_val);
        self->custom_package_val = custom_val;

    }
    return 0;
}

/* getsetters */

static PyObject *
get_cache_dir(_SackObject *self, void *unused)
{
    const char *cstr = hif_sack_get_cache_dir(self->sack);
    if (cstr == NULL)
        Py_RETURN_NONE;
    return PyString_FromString(cstr);
}

static int
set_installonly(_SackObject *self, PyObject *obj, void *unused)
{
    if (!PySequence_Check(obj)) {
        PyErr_SetString(PyExc_AttributeError, "Expected a sequence.");
        return -1;
    }

    const int len = PySequence_Length(obj);
    const char *strings[len + 1];
    PyObject *tmp_py_str[len];

    for (int i = 0; i < len; ++i) {
        PyObject *item = PySequence_GetItem(obj, i);
        tmp_py_str[i] = NULL;
        strings[i] = NULL;
        if (PyUnicode_Check(item) || PyString_Check(item)) {
            strings[i] = pycomp_get_string(item, &tmp_py_str[i]);
        }
        Py_DECREF(item);
        if (strings[i] == NULL) {
            pycomp_free_tmp_array(tmp_py_str, i);
            return -1;
        }
    }
    strings[len] = NULL;

    HifSack *sack = self->sack;
    hif_sack_set_installonly(sack, strings);
    pycomp_free_tmp_array(tmp_py_str, len - 1);

    return 0;
}

static int
set_installonly_limit(_SackObject *self, PyObject *obj, void *unused)
{
    int limit = (int)PyLong_AsLong(obj);
    if (PyErr_Occurred())
        return -1;
    hif_sack_set_installonly_limit(self->sack, limit);
    return 0;
}

static PyGetSetDef sack_getsetters[] = {
    {(char*)"cache_dir",        (getter)get_cache_dir, NULL, NULL, NULL},
    {(char*)"installonly",        NULL, (setter)set_installonly, NULL, NULL},
    {(char*)"installonly_limit",        NULL, (setter)set_installonly_limit, NULL, NULL},
    {NULL}                        /* sentinel */
};

/* object methods */

static PyObject *
_knows(_SackObject *self, PyObject *args, PyObject *kwds)
{
    const char *name;
    const char *version = NULL;
    int name_only = 0, icase = 0, glob = 0;

    const char *kwlist[] = {"name", "version", "name_only", "icase", "glob",
                      NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|ziii", (char**) kwlist,
                                     &name, &version, &name_only, &icase, &glob))
        return NULL;

    int flags = 0;
    if (name_only)
        flags |= HY_NAME_ONLY;
    if (icase)
        flags |= HY_ICASE;
    if (glob)
        flags |= HY_GLOB;
    return PyLong_FromLong(hif_sack_knows(self->sack, name, version, flags));
}

static PyObject *
evr_cmp(_SackObject *self, PyObject *args)
{
    const char *evr1 = NULL, *evr2 = NULL;

    if (!PyArg_ParseTuple(args, "ss", &evr1, &evr2))
        return NULL;
    int cmp = hif_sack_evr_cmp(self->sack, evr1, evr2);
    return PyLong_FromLong(cmp);
}

static PyObject *
get_running_kernel(_SackObject *self, PyObject *unused)
{
    HifSack *sack = self->sack;
    HifPackage *cpkg = hif_sack_get_running_kernel(sack);

    if (cpkg == NULL)
        Py_RETURN_NONE;
    PyObject *pkg = new_package((PyObject*)self, hif_package_get_id(cpkg));
    g_object_unref(cpkg);
    return pkg;
}

static PyObject *
create_cmdline_repo(_SackObject *self, PyObject *unused)
{
    Py_RETURN_NONE;
}

static PyObject *
create_package(_SackObject *self, PyObject *solvable_id)
{
    Id id  = PyLong_AsLong(solvable_id);
    if (id <= 0) {
        PyErr_SetString(PyExc_TypeError, "Expected a positive integer.");
        return NULL;
    }
    return new_package((PyObject*)self, id);
}

static PyObject *
add_cmdline_package(_SackObject *self, PyObject *fn_obj)
{
    HifPackage *cpkg;
    PyObject *pkg;
    PyObject *tmp_py_str = NULL;
    const char *fn = pycomp_get_string(fn_obj, &tmp_py_str);

    if (fn == NULL) {
        Py_XDECREF(tmp_py_str);
        return NULL;
    }
    cpkg = hif_sack_add_cmdline_package(self->sack, fn);
    Py_XDECREF(tmp_py_str);
    if (cpkg == NULL) {
        PyErr_Format(PyExc_IOError, "Can not load RPM file: %s.", fn);
        return NULL;
    }
    pkg = new_package((PyObject*)self, hif_package_get_id(cpkg));
    g_object_unref(cpkg);
    return pkg;
}

static PyObject *
add_excludes(_SackObject *self, PyObject *seq)
{
    HifSack *sack = self->sack;
    HifPackageSet *pset = pyseq_to_packageset(seq, sack);
    if (pset == NULL)
        return NULL;
    hif_sack_add_excludes(sack, pset);
    g_object_unref(pset);
    Py_RETURN_NONE;
}

static PyObject *
add_includes(_SackObject *self, PyObject *seq)
{
    HifSack *sack = self->sack;
    HifPackageSet *pset = pyseq_to_packageset(seq, sack);
    if (pset == NULL)
        return NULL;
    hif_sack_add_includes(sack, pset);
    g_object_unref(pset);
    Py_RETURN_NONE;
}

static PyObject *
disable_repo(_SackObject *self, PyObject *reponame)
{
    return repo_enabled(self, reponame, 0);
}

static PyObject *
enable_repo(_SackObject *self, PyObject *reponame)
{
    return repo_enabled(self, reponame, 1);
}

static PyObject *
list_arches(_SackObject *self, PyObject *unused)
{
    const char **arches = hif_sack_list_arches(self->sack);
    PyObject *list;
    if (!arches) {
        PyErr_SetString(HyExc_Runtime, "Arches not initialized");
        return NULL;
    }
    list = strlist_to_pylist(arches);
    g_free(arches);
    return list;
}

static PyObject *
load_system_repo(_SackObject *self, PyObject *args, PyObject *kwds)
{
    g_autoptr(GError) error = NULL;
    const char *kwlist[] = {"repo", "build_cache", "load_filelists", "load_presto",
                      NULL};

    HyRepo crepo = NULL;
    int build_cache = 0, unused_1 = 0, unused_2 = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O&iii", (char**) kwlist,
                                     repo_converter, &crepo,
                                     &build_cache, &unused_1, &unused_2))


        return 0;

    int flags = 0;
    if (build_cache)
        flags |= HIF_SACK_LOAD_FLAG_BUILD_CACHE;

    gboolean ret = hif_sack_load_system_repo(self->sack, crepo, flags, &error);
    if (!ret)
        return op_error2exc(error);
    Py_RETURN_NONE;
}

static PyObject *
load_repo(_SackObject *self, PyObject *args, PyObject *kwds)
{
    const char *kwlist[] = {"repo", "build_cache", "load_filelists", "load_presto",
                      "load_updateinfo", NULL};

    HyRepo crepo = NULL;
    int build_cache = 0, load_filelists = 0, load_presto = 0, load_updateinfo = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&|iiii", (char**) kwlist,
                                     repo_converter, &crepo,
                                     &build_cache, &load_filelists,
                                     &load_presto, &load_updateinfo))
        return 0;

    int flags = 0;
    gboolean ret = 0;
    g_autoptr(GError) error = NULL;
    if (build_cache)
        flags |= HIF_SACK_LOAD_FLAG_BUILD_CACHE;
    if (load_filelists)
        flags |= HIF_SACK_LOAD_FLAG_USE_FILELISTS;
    if (load_presto)
        flags |= HIF_SACK_LOAD_FLAG_USE_PRESTO;
    if (load_updateinfo)
        flags |= HIF_SACK_LOAD_FLAG_USE_UPDATEINFO;
    Py_BEGIN_ALLOW_THREADS;
    ret = hif_sack_load_repo(self->sack, crepo, flags, &error);
    Py_END_ALLOW_THREADS;
    if (!ret)
        return op_error2exc(error);
    Py_RETURN_NONE;
}

static Py_ssize_t
len(_SackObject *self)
{
    return hif_sack_count(self->sack);
}

static PyObject *
deepcopy(_SackObject *self, PyObject *args, PyObject *kwds)
{
    PyErr_SetString(PyExc_NotImplementedError, "sack can't be deepcopied");
    return NULL;
}

static struct
PyMethodDef sack_methods[] = {
    {"__deepcopy__", (PyCFunction)deepcopy, METH_KEYWORDS|METH_VARARGS,
     NULL},
    {"_knows",                (PyCFunction)_knows, METH_KEYWORDS|METH_VARARGS,
     NULL},
    {"evr_cmp",                (PyCFunction)evr_cmp, METH_VARARGS,
     NULL},
    {"get_running_kernel", (PyCFunction)get_running_kernel, METH_NOARGS,
     NULL},
    {"create_cmdline_repo", (PyCFunction)create_cmdline_repo, METH_NOARGS,
     NULL},
    {"create_package", (PyCFunction)create_package, METH_O,
     NULL},
    {"add_cmdline_package", (PyCFunction)add_cmdline_package, METH_O,
     NULL},
    {"add_excludes", (PyCFunction)add_excludes, METH_O,
     NULL},
    {"add_includes", (PyCFunction)add_includes, METH_O,
     NULL},
    {"disable_repo", (PyCFunction)disable_repo, METH_O,
     NULL},
    {"enable_repo", (PyCFunction)enable_repo, METH_O,
     NULL},
    {"list_arches", (PyCFunction)list_arches, METH_NOARGS,
     NULL},
    {"load_system_repo", (PyCFunction)load_system_repo,
     METH_VARARGS | METH_KEYWORDS, NULL},
    {"load_repo", (PyCFunction)load_repo, METH_VARARGS | METH_KEYWORDS,
     NULL},
    {"load_yum_repo", (PyCFunction)load_repo, METH_VARARGS | METH_KEYWORDS,
     NULL},
    {NULL}                      /* sentinel */
};

PySequenceMethods sack_sequence = {
    (lenfunc)len,                /* sq_length */
};

PyTypeObject sack_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_hawkey.Sack",                /*tp_name*/
    sizeof(_SackObject),        /*tp_basicsize*/
    0,                                /*tp_itemsize*/
    (destructor) sack_dealloc,  /*tp_dealloc*/
    0,                                /*tp_print*/
    0,                                /*tp_getattr*/
    0,                                /*tp_setattr*/
    0,                                /*tp_compare*/
    0,                                /*tp_repr*/
    0,                                /*tp_as_number*/
    &sack_sequence,                /*tp_as_sequence*/
    0,                                /*tp_as_mapping*/
    0,                                /*tp_hash */
    0,                                /*tp_call*/
    0,                                /*tp_str*/
    0,                                /*tp_getattro*/
    0,                                /*tp_setattro*/
    0,                                /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE,        /*tp_flags*/
    "Sack object",                /* tp_doc */
    0,                                /* tp_traverse */
    0,                                /* tp_clear */
    0,                                /* tp_richcompare */
    0,                                /* tp_weaklistoffset */
    PyObject_SelfIter,                /* tp_iter */
    0,                                 /* tp_iternext */
    sack_methods,                /* tp_methods */
    0,                                /* tp_members */
    sack_getsetters,                /* tp_getset */
    0,                                /* tp_base */
    0,                                /* tp_dict */
    0,                                /* tp_descr_get */
    0,                                /* tp_descr_set */
    0,                                /* tp_dictoffset */
    (initproc)sack_init,        /* tp_init */
    0,                                /* tp_alloc */
    sack_new,                        /* tp_new */
    0,                                /* tp_free */
    0,                                /* tp_is_gc */
};
