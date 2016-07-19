/*
 * Copyright (C) 2014 Red Hat, Inc.
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

#include <Python.h>
#include <datetime.h>

// hawkey
#include "hif-advisory-private.h"
#include "hif-advisorypkg.h"
#include "hif-advisoryref.h"
#include "hy-package.h"

// pyhawkey
#include "advisory-py.h"
#include "iutil-py.h"

#include "pycomp.h"

typedef struct {
    PyObject_HEAD
    HifAdvisory *advisory;
    PyObject *sack;
} _AdvisoryObject;


PyObject *
advisoryToPyObject(HifAdvisory *advisory, PyObject *sack)
{
    _AdvisoryObject *self = PyObject_New(_AdvisoryObject, &advisory_Type);
    if (!self)
        return NULL;

    self->advisory = advisory;
    self->sack = sack;
    Py_INCREF(sack);

    return (PyObject *)self;
}

static HifAdvisory *
advisoryFromPyObject(PyObject *o)
{
    if (!PyObject_TypeCheck(o, &advisory_Type)) {
        PyErr_SetString(PyExc_TypeError, "Expected an Advisory object.");
        return NULL;
    }
    return ((_AdvisoryObject*)o)->advisory;
}

static int
advisory_converter(PyObject *o, HifAdvisory **advisory_ptr)
{
    HifAdvisory *advisory = advisoryFromPyObject(o);
    if (advisory == NULL)
        return 0;
    *advisory_ptr = advisory;
    return 1;
}

/* functions on the type */

static void
advisory_dealloc(_AdvisoryObject *self)
{
    g_object_unref(self->advisory);
    Py_XDECREF(self->sack);
    Py_TYPE(self)->tp_free(self);
}

static PyObject *
advisory_richcompare(PyObject *self, PyObject *other, int op)
{
    PyObject *result;
    HifAdvisory *cself, *cother;

    if (!advisory_converter(self, &cself) ||
        !advisory_converter(other, &cother)) {
        if(PyErr_Occurred() && PyErr_ExceptionMatches(PyExc_TypeError))
            PyErr_Clear();
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }

    int identical = hif_advisory_compare(cself, cother);
    switch (op) {
    case Py_EQ:
        result = TEST_COND(identical);
        break;
    case Py_NE:
        result = TEST_COND(!identical);
        break;
    case Py_LE:
    case Py_GE:
    case Py_LT:
    case Py_GT:
        result = Py_NotImplemented;
        break;
    default:
        PyErr_BadArgument();
        return NULL;
    }

    Py_INCREF(result);
    return result;
}

/* getsetters */

static PyObject *
get_str(_AdvisoryObject *self, void *closure)
{
    const char *(*func)(HifAdvisory*);
    const char *cstr;

    func = (const char *(*)(HifAdvisory*))closure;
    cstr = func(self->advisory);
    if (cstr == NULL)
        Py_RETURN_NONE;
    return PyUnicode_FromString(cstr);
}

static PyObject *
get_type(_AdvisoryObject *self, void *closure)
{
    HifAdvisoryKind (*func)(HifAdvisory*);
    HifAdvisoryKind ctype;

    func = (HifAdvisoryKind (*)(HifAdvisory*))closure;
    ctype = func(self->advisory);
    return PyLong_FromLong(ctype);
}

static PyObject *
get_datetime(_AdvisoryObject *self, void *closure)
{
    guint64 (*func)(HifAdvisory*);
    func = (guint64 (*)(HifAdvisory*))closure;
    PyObject *timestamp = PyLong_FromUnsignedLongLong(func(self->advisory));
    PyObject *args = Py_BuildValue("(O)", timestamp);
    PyDateTime_IMPORT;
    PyObject *datetime = PyDateTime_FromTimestamp(args);
    Py_DECREF(args);
    Py_DECREF(timestamp);
    return datetime;
}

static PyObject *
get_advisorypkg_list(_AdvisoryObject *self, void *closure)
{
    GPtrArray *(*func)(HifAdvisory*);
    GPtrArray *advisorypkgs;
    PyObject *list;

    func = (GPtrArray *(*)(HifAdvisory*))closure;
    advisorypkgs = func(self->advisory);
    if (advisorypkgs == NULL)
        Py_RETURN_NONE;

    list = advisorypkglist_to_pylist(advisorypkgs);
    g_ptr_array_unref(advisorypkgs);

    return list;
}

static PyObject *
get_advisoryref_list(_AdvisoryObject *self, void *closure)
{
    GPtrArray *(*func)(HifAdvisory*);
    GPtrArray *advisoryrefs;
    PyObject *list;

    func = (GPtrArray *(*)(HifAdvisory*))closure;
    advisoryrefs = func(self->advisory);
    if (advisoryrefs == NULL)
        Py_RETURN_NONE;

    list = advisoryreflist_to_pylist(advisoryrefs, self->sack);
    g_ptr_array_unref(advisoryrefs);

    return list;
}

static PyGetSetDef advisory_getsetters[] = {
    {(char*)"title", (getter)get_str, NULL, NULL, (void *)hif_advisory_get_title},
    {(char*)"id", (getter)get_str, NULL, NULL, (void *)hif_advisory_get_id},
    {(char*)"type", (getter)get_type, NULL, NULL, (void *)hif_advisory_get_kind},
    {(char*)"description", (getter)get_str, NULL, NULL, (void *)hif_advisory_get_description},
    {(char*)"rights", (getter)get_str, NULL, NULL, (void *)hif_advisory_get_rights},
    {(char*)"updated", (getter)get_datetime, NULL, NULL, (void *)hif_advisory_get_updated},
    {(char*)"packages", (getter)get_advisorypkg_list, NULL, NULL, (void *)hif_advisory_get_packages},
    {(char*)"references", (getter)get_advisoryref_list, NULL, NULL, (void *)hif_advisory_get_references},
    {NULL}                      /* sentinel */
};

PyTypeObject advisory_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_hawkey.Advisory",                /*tp_name*/
    sizeof(_AdvisoryObject),        /*tp_basicsize*/
    0,                                /*tp_itemsize*/
    (destructor) advisory_dealloc,        /*tp_dealloc*/
    0,                                /*tp_print*/
    0,                                /*tp_getattr*/
    0,                                /*tp_setattr*/
    0,                                /*tp_compare*/
    0,                                /*tp_repr*/
    0,                                /*tp_as_number*/
    0,                                /*tp_as_sequence*/
    0,                                /*tp_as_mapping*/
    0,                                /*tp_hash */
    0,                                /*tp_call*/
    0,                                /*tp_str*/
    0,                                /*tp_getattro*/
    0,                                /*tp_setattro*/
    0,                                /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE,        /*tp_flags*/
    "Advisory object",                /* tp_doc */
    0,                                /* tp_traverse */
    0,                                /* tp_clear */
    advisory_richcompare,        /* tp_richcompare */
    0,                                /* tp_weaklistoffset */
    0,                                /* tp_iter */
    0,                                /* tp_iternext */
    0,                                /* tp_methods */
    0,                                /* tp_members */
    advisory_getsetters,        /* tp_getset */
    0,                                /* tp_base */
    0,                                /* tp_dict */
    0,                                /* tp_descr_get */
    0,                                /* tp_descr_set */
    0,                                /* tp_dictoffset */
    0,                                /* tp_init */
    0,                                /* tp_alloc */
    0,                                /* tp_new */
    0,                                /* tp_free */
    0,                                /* tp_is_gc */
};
