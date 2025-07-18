/*
   Unix SMB/CIFS implementation.
   Samba utility functions
   Copyright (C) Matthieu Patou <mat@matws.net> 2010

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "lib/replace/system/python.h"

static void PyType_AddMethods(PyTypeObject *type, PyMethodDef *methods)
{
        PyObject *dict;
        int i;
        if (type->tp_dict == NULL)
                type->tp_dict = PyDict_New();
        dict = type->tp_dict;
        for (i = 0; methods[i].ml_name; i++) {
                PyObject *descr;
                if (methods[i].ml_flags & METH_CLASS)
                        descr = PyCFunction_New(&methods[i], (PyObject *)type);
                else
                        descr = PyDescr_NewMethod(type, &methods[i]);
                PyDict_SetItemString(dict, methods[i].ml_name,
                                     descr);
		Py_CLEAR(descr);
        }
}

static void ntacl_print_debug_helper(struct ndr_print *ndr, const char *format, ...) PRINTF_ATTRIBUTE(2,3);

static void ntacl_print_debug_helper(struct ndr_print *ndr, const char *format, ...)
{
        va_list ap;
        char *s = NULL;
        int ret;

        va_start(ap, format);
        ret = vasprintf(&s, format, ap);
        va_end(ap);

	if (ret == -1) {
		return;
	}

	printf("%*.s\n", 4 * ndr->depth, s);

        free(s);
}

static PyObject *py_ntacl_print(PyObject *self, PyObject *args)
{
	struct xattr_NTACL *ntacl = pytalloc_get_ptr(self);
	struct ndr_print *pr;
	TALLOC_CTX *mem_ctx;

	mem_ctx = talloc_new(NULL);

	pr = talloc_zero(mem_ctx, struct ndr_print);
	if (!pr) {
		PyErr_NoMemory();
		talloc_free(mem_ctx);
		return NULL;
	}
	pr->print = ntacl_print_debug_helper;
	ndr_print_xattr_NTACL(pr, "file", ntacl);

	talloc_free(mem_ctx);

	Py_RETURN_NONE;
}

static PyMethodDef py_ntacl_extra_methods[] = {
	{ "dump", (PyCFunction)py_ntacl_print, METH_NOARGS,
		NULL },
	{0}
};

static void py_xattr_NTACL_patch(PyTypeObject *type)
{
	PyType_AddMethods(type, py_ntacl_extra_methods);
}

#define PY_NTACL_PATCH py_xattr_NTACL_patch


