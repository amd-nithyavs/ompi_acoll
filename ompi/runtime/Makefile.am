#
# Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2018 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2006-2015 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
# Copyright (c) 2016      IBM Corporation.  All rights reserved.
# Copyright (c) 2025      Jeffrey M. Squyres.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# This makefile.am does not stand on its own - it is included from ompi/Makefile.am

EXTRA_DIST += runtime/help-mpi-runtime.txt

headers += \
        runtime/mpiruntime.h \
        runtime/params.h \
	runtime/ompi_info_support.h \
	runtime/ompi_spc.h \
	runtime/ompi_rte.h

lib@OMPI_LIBMPI_NAME@_la_SOURCES += \
        runtime/ompi_mpi_init.c \
        runtime/ompi_mpi_abort.c \
        runtime/ompi_mpi_dynamics.c \
        runtime/ompi_mpi_finalize.c \
        runtime/ompi_mpi_params.c \
        runtime/ompi_mpi_preconnect.c \
	runtime/ompi_info_support.c \
	runtime/ompi_spc.c \
	runtime/ompi_rte.c
