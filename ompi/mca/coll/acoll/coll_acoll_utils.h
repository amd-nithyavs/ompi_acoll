/* -*- Mode: C; indent-tabs-mode:nil -*- */
/*
 * Copyright (C) 2024, Advanced Micro Devices, Inc. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "ompi_config.h"

#include "mpi.h"
#include "ompi/communicator/communicator.h"
#include "ompi/mca/coll/base/coll_base_functions.h"
#include "opal/include/opal/align.h"

static inline void *coll_acoll_malloc(coll_acoll_reserve_mem_t *reserve_mem_ptr, int64_t size)
{
    void *temp_ptr = NULL;
    if ((true == reserve_mem_ptr->reserve_mem_allocate)
        && (size <= reserve_mem_ptr->reserve_mem_size)
        && (false == reserve_mem_ptr->reserve_mem_in_use)) {
        if (NULL == reserve_mem_ptr->reserve_mem) {
            reserve_mem_ptr->reserve_mem = malloc(reserve_mem_ptr->reserve_mem_size);
        }
        temp_ptr = reserve_mem_ptr->reserve_mem;

        if (NULL != temp_ptr) {
            reserve_mem_ptr->reserve_mem_in_use = true;
        }
    } else {
        temp_ptr = malloc(size);
    }

    return temp_ptr;
}

static inline void coll_acoll_free(coll_acoll_reserve_mem_t *reserve_mem_ptr, void *ptr)
{
    if ((false == reserve_mem_ptr->reserve_mem_allocate)
        || (false == reserve_mem_ptr->reserve_mem_in_use)) {
        if (NULL != ptr) {
            free(ptr);
        }
    } else if (reserve_mem_ptr->reserve_mem == ptr) {
        /* Guard to ensure reserve mem is referenced correctly */
        reserve_mem_ptr->reserve_mem_in_use = false;
    }
}

static inline int log_sg_bcast_intra(void *buff, int count, struct ompi_datatype_t *datatype,
                                     int rank, int dim, int size, int sg_size, int cur_base,
                                     int sg_start, struct ompi_communicator_t *comm,
                                     mca_coll_base_module_t *module, ompi_request_t **preq,
                                     int *nreqs)
{
    int msb_pos, sub_rank, peer, err;
    int i, mask;
    int end_sg, end_peer;

    end_sg = sg_start + sg_size - 1;
    if (end_sg >= size) {
        end_sg = size - 1;
    }
    end_peer = (end_sg - cur_base) % sg_size;
    sub_rank = (rank - cur_base + sg_size) % sg_size;

    msb_pos = opal_hibit(sub_rank, dim);
    --dim;

    /* Receive data from parent in the sg tree. */
    if (sub_rank > 0) {
        assert(msb_pos >= 0);
        peer = (sub_rank & ~(1 << msb_pos));
        if (peer > end_peer) {
            peer = (((peer + cur_base - sg_start) % sg_size) + sg_start);
        } else {
            peer = peer + cur_base;
        }

        err = MCA_PML_CALL(
            recv(buff, count, datatype, peer, MCA_COLL_BASE_TAG_BCAST, comm, MPI_STATUS_IGNORE));
        if (MPI_SUCCESS != err) {
            return err;
        }
    }

    for (i = msb_pos + 1, mask = 1 << i; i <= dim; ++i, mask <<= 1) {
        peer = sub_rank | mask;
        if (peer >= sg_size) {
            continue;
        }
        if (peer >= end_peer) {
            peer = (((peer + cur_base - sg_start) % sg_size) + sg_start);
        } else {
            peer = peer + cur_base;
        }
        if ((peer < size) && (peer != rank) && (peer != cur_base)) {
            *nreqs = *nreqs + 1;
            err = MCA_PML_CALL(isend(buff, count, datatype, peer, MCA_COLL_BASE_TAG_BCAST,
                                     MCA_PML_BASE_SEND_STANDARD, comm, preq++));
            if (MPI_SUCCESS != err) {
                return err;
            }
        }
    }

    return err;
}

static inline int lin_sg_bcast_intra(void *buff, int count, struct ompi_datatype_t *datatype,
                                     int rank, int dim, int size, int sg_size, int cur_base,
                                     int sg_start, struct ompi_communicator_t *comm,
                                     mca_coll_base_module_t *module, ompi_request_t **preq,
                                     int *nreqs)
{
    int peer;
    int err;
    int sg_end;

    sg_end = sg_start + sg_size - 1;
    if (sg_end >= size) {
        sg_end = size - 1;
    }

    if (rank == cur_base) {
        for (peer = sg_start; peer <= sg_end; peer++) {
            if (peer == cur_base) {
                continue;
            }
            *nreqs = *nreqs + 1;
            err = MCA_PML_CALL(isend(buff, count, datatype, peer, MCA_COLL_BASE_TAG_BCAST,
                                     MCA_PML_BASE_SEND_STANDARD, comm, preq++));
            if (MPI_SUCCESS != err) {
                return err;
            }
        }
    } else {
        err = MCA_PML_CALL(recv(buff, count, datatype, cur_base, MCA_COLL_BASE_TAG_BCAST, comm,
                                MPI_STATUS_IGNORE));
        if (MPI_SUCCESS != err) {
            return err;
        }
    }

    return err;
}

/*
 * sg_bcast_intra
 *
 * Function:    broadcast operation within a subgroup
 * Accepts:     Arguments of MPI_Bcast() plus subgroup params
 * Returns:     MPI_SUCCESS or error code
 *
 * Description: O(N) or O(log(N)) algorithm based on count.
 *
 * Memory:      No additional memory requirements beyond user-supplied buffers.
 *
 */
static inline int sg_bcast_intra(void *buff, int count, struct ompi_datatype_t *datatype, int rank,
                                 int dim, int size, int sg_size, int cur_base, int sg_start,
                                 struct ompi_communicator_t *comm, mca_coll_base_module_t *module,
                                 ompi_request_t **preq, int *nreqs)
{
    int err;
    size_t total_dsize, dsize;

    ompi_datatype_type_size(datatype, &dsize);
    total_dsize = dsize * (unsigned long) count;

    if (total_dsize <= 8192) {
        err = log_sg_bcast_intra(buff, count, datatype, rank, dim, size, sg_size, cur_base,
                                 sg_start, comm, module, preq, nreqs);
    } else {
        err = lin_sg_bcast_intra(buff, count, datatype, rank, dim, size, sg_size, cur_base,
                                 sg_start, comm, module, preq, nreqs);
    }
    return err;
}

static int compare_cids(const void *ptra, const void *ptrb)
{
    int a = *((int *) ptra);
    int b = *((int *) ptrb);

    if (a < b) {
        return -1;
    } else if (a > b) {
        return 1;
    }

    return 0;
}

static inline int comm_grp_ranks_local(ompi_communicator_t *comm, ompi_communicator_t *local_comm,
                                       int *is_root_node, int *local_root, int **ranks_buf,
                                       int root)
{
    ompi_group_t *local_grp, *grp;
    int local_size = ompi_comm_size(local_comm);
    int *ranks = malloc(local_size * sizeof(int));
    int *local_ranks = malloc(local_size * sizeof(int));
    int i, err;

    err = ompi_comm_group(comm, &grp);
    err = ompi_comm_group(local_comm, &local_grp);

    for (i = 0; i < local_size; i++) {
        local_ranks[i] = i;
    }

    err = ompi_group_translate_ranks(local_grp, local_size, local_ranks, grp, ranks);
    if (ranks_buf != NULL) {
        *ranks_buf = malloc(local_size * sizeof(int));
        memcpy(*ranks_buf, ranks, local_size * sizeof(int));
    }

    for (i = 0; i < local_size; i++) {
        if (ranks[i] == root) {
            *is_root_node = 1;
            *local_root = i;
            break;
        }
    }

    err = ompi_group_free(&grp);
    err = ompi_group_free(&local_grp);
    free(ranks);
    free(local_ranks);

    return err;
}

static inline int mca_coll_acoll_create_base_comm(ompi_communicator_t **parent_comm,
                                                  coll_acoll_subcomms_t *subc, int color, int rank,
                                                  int *root, int base_lyr)
{
    int i;
    int err;

    for (i = 0; i < MCA_COLL_ACOLL_NUM_LAYERS; i++) {
        int is_root_node = 0;

        /* Create base comm */
        err = ompi_comm_split(parent_comm[i], color, rank, &subc->base_comm[base_lyr][i], false);
        if (MPI_SUCCESS != err)
            return err;

        /* Find out local rank of root in base comm */
        err = comm_grp_ranks_local(parent_comm[i], subc->base_comm[base_lyr][i], &is_root_node,
                                   &subc->base_root[base_lyr][i], NULL, root[i]);
    }
    return err;
}

static inline int mca_coll_acoll_comm_split_init(ompi_communicator_t *comm,
                                                 mca_coll_acoll_module_t *acoll_module, int root)
{
    opal_info_t comm_info;
    mca_coll_base_module_allreduce_fn_t coll_allreduce_org = (comm)->c_coll->coll_allreduce;
    mca_coll_base_module_allgather_fn_t coll_allgather_org = (comm)->c_coll->coll_allgather;
    mca_coll_base_module_bcast_fn_t coll_bcast_org = (comm)->c_coll->coll_bcast;
    mca_coll_base_module_allreduce_fn_t coll_allreduce_loc, coll_allreduce_soc;
    mca_coll_base_module_allgather_fn_t coll_allgather_loc, coll_allgather_soc;
    mca_coll_base_module_bcast_fn_t coll_bcast_loc, coll_bcast_soc;
    coll_acoll_subcomms_t *subc;
    int err;
    int size = ompi_comm_size(comm);
    int rank = ompi_comm_rank(comm);
    int cid = ompi_comm_get_local_cid(comm);
    if (cid >= MCA_COLL_ACOLL_MAX_CID) {
        return MPI_SUCCESS;
    }

    /* Derive subcomm structure */
    subc = &acoll_module->subc[cid];
    subc->cid = cid;
    subc->orig_comm = comm;

    (comm)->c_coll->coll_allgather = ompi_coll_base_allgather_intra_ring;
    (comm)->c_coll->coll_allreduce = ompi_coll_base_allreduce_intra_recursivedoubling;
    (comm)->c_coll->coll_bcast = ompi_coll_base_bcast_intra_basic_linear;
    if (!subc->initialized) {
        OBJ_CONSTRUCT(&comm_info, opal_info_t);
        opal_info_set(&comm_info, "ompi_comm_coll_preference", "libnbc,basic,^acoll");
        /* Create node-level subcommunicator */
        err = ompi_comm_split_type(comm, MPI_COMM_TYPE_SHARED, 0, &comm_info, &(subc->local_comm));
        if (MPI_SUCCESS != err) {
            return err;
        }
        /* Create socket-level subcommunicator */
        err = ompi_comm_split_type(comm, OMPI_COMM_TYPE_SOCKET, 0, &comm_info,
                                   &(subc->socket_comm));
        if (MPI_SUCCESS != err) {
            return err;
        }
        OBJ_DESTRUCT(&comm_info);
        OBJ_CONSTRUCT(&comm_info, opal_info_t);
        opal_info_set(&comm_info, "ompi_comm_coll_preference", "libnbc,basic,^acoll");
        /* Create subgroup-level subcommunicator */
        err = ompi_comm_split_type(comm, OMPI_COMM_TYPE_L3CACHE, 0, &comm_info,
                                   &(subc->subgrp_comm));
        if (MPI_SUCCESS != err) {
            return err;
        }
        err = ompi_comm_split_type(comm, OMPI_COMM_TYPE_NUMA, 0, &comm_info, &(subc->numa_comm));
        if (MPI_SUCCESS != err) {
            return err;
        }
        subc->subgrp_size = ompi_comm_size(subc->subgrp_comm);
        OBJ_DESTRUCT(&comm_info);

        /* Derive the no. of nodes */
        if (size == ompi_comm_size(subc->local_comm)) {
            subc->num_nodes = 1;
        } else {
            int *size_list_buf = (int *) malloc(size * sizeof(int));
            int num_nodes = 0;
            int local_size = ompi_comm_size(subc->local_comm);
            err = (comm)->c_coll->coll_allgather(&local_size, 1, MPI_INT, size_list_buf, 1, MPI_INT,
                                                 comm, &acoll_module->super);
            if (MPI_SUCCESS != err) {
                free(size_list_buf);
                return err;
            }
            /* Sort the size array */
            qsort(size_list_buf, size, sizeof(int), compare_cids);
            /* Find the no. of nodes */
            for (int i = 0; i < size;) {
                int ofst = size_list_buf[i];
                num_nodes++;
                i += ofst;
            }
            subc->num_nodes = num_nodes;
            free(size_list_buf);
        }
    }
    /* Common initializations */
    {
        subc->outer_grp_root = -1;
        subc->subgrp_root = 0;
        subc->is_root_sg = 0;
        subc->is_root_numa = 0;
        subc->numa_root = 0;
        subc->is_root_socket = 0;
        subc->socket_ldr_root = -1;

        if (subc->initialized) {
            if (subc->num_nodes > 1) {
                ompi_comm_free(&(subc->leader_comm));
                subc->leader_comm = NULL;
            }
            ompi_comm_free(&(subc->socket_ldr_comm));
            subc->socket_ldr_comm = NULL;
        }
        for (int i = 0; i < MCA_COLL_ACOLL_NUM_LAYERS; i++) {
            if (subc->initialized) {
                ompi_comm_free(&(subc->base_comm[MCA_COLL_ACOLL_L3CACHE][i]));
                subc->base_comm[MCA_COLL_ACOLL_L3CACHE][i] = NULL;
                ompi_comm_free(&(subc->base_comm[MCA_COLL_ACOLL_NUMA][i]));
                subc->base_comm[MCA_COLL_ACOLL_NUMA][i] = NULL;
            }
            subc->base_root[MCA_COLL_ACOLL_L3CACHE][i] = -1;
            subc->base_root[MCA_COLL_ACOLL_NUMA][i] = -1;
        }
        /* Store original collectives for local and socket comms */
        coll_allreduce_loc = (subc->local_comm)->c_coll->coll_allreduce;
        coll_allgather_loc = (subc->local_comm)->c_coll->coll_allgather;
        coll_bcast_loc = (subc->local_comm)->c_coll->coll_bcast;
        (subc->local_comm)->c_coll->coll_allgather = ompi_coll_base_allgather_intra_ring;
        (subc->local_comm)->c_coll->coll_allreduce
            = ompi_coll_base_allreduce_intra_recursivedoubling;
        (subc->local_comm)->c_coll->coll_bcast = ompi_coll_base_bcast_intra_basic_linear;
        coll_allreduce_soc = (subc->socket_comm)->c_coll->coll_allreduce;
        coll_allgather_soc = (subc->socket_comm)->c_coll->coll_allgather;
        coll_bcast_soc = (subc->socket_comm)->c_coll->coll_bcast;
        (subc->socket_comm)->c_coll->coll_allgather = ompi_coll_base_allgather_intra_ring;
        (subc->socket_comm)->c_coll->coll_allreduce
            = ompi_coll_base_allreduce_intra_recursivedoubling;
        (subc->socket_comm)->c_coll->coll_bcast = ompi_coll_base_bcast_intra_basic_linear;
    }

    /* Further subcommunicators based on root */
    if (subc->num_nodes > 1) {
        int local_rank = ompi_comm_rank(subc->local_comm);
        int color = MPI_UNDEFINED;
        int is_root_node = 0, is_root_socket = 0;
        int local_root = 0;
        int *subgrp_ranks = NULL, *numa_ranks = NULL, *socket_ranks = NULL;
        ompi_communicator_t *parent_comm[MCA_COLL_ACOLL_NUM_LAYERS];

        /* Initializations */
        subc->local_root[MCA_COLL_ACOLL_LYR_NODE] = 0;
        subc->local_root[MCA_COLL_ACOLL_LYR_SOCKET] = 0;

        /* Find out the local rank of root */
        err = comm_grp_ranks_local(comm, subc->local_comm, &subc->is_root_node,
                                   &subc->local_root[MCA_COLL_ACOLL_LYR_NODE], NULL, root);

        /* Create subcommunicator with leader ranks */
        color = 1;
        if (!subc->is_root_node && (local_rank == 0)) {
            color = 0;
        }
        if (rank == root) {
            color = 0;
        }
        err = ompi_comm_split(comm, color, rank, &subc->leader_comm, false);
        if (MPI_SUCCESS != err) {
            return err;
        }

        /* Find out local rank of root in leader comm */
        err = comm_grp_ranks_local(comm, subc->leader_comm, &is_root_node, &subc->outer_grp_root,
                                   NULL, root);

        /* Find out local rank of root in socket comm */
        if (subc->is_root_node) {
            local_root = subc->local_root[MCA_COLL_ACOLL_LYR_NODE];
        }
        err = comm_grp_ranks_local(subc->local_comm, subc->socket_comm, &subc->is_root_socket,
                                   &subc->local_root[MCA_COLL_ACOLL_LYR_SOCKET], &socket_ranks,
                                   local_root);

        /* Create subcommunicator with socket leaders */
        subc->socket_rank = subc->is_root_socket == 1 ? local_root : socket_ranks[0];
        color = local_rank == subc->socket_rank ? 0 : 1;
        err = ompi_comm_split(subc->local_comm, color, local_rank, &subc->socket_ldr_comm, false);
        if (MPI_SUCCESS != err)
            return err;

        /* Find out local rank of root in socket leader comm */
        err = comm_grp_ranks_local(subc->local_comm, subc->socket_ldr_comm, &is_root_socket,
                                   &subc->socket_ldr_root, NULL, local_root);

        /* Find out local rank of root in subgroup comm */
        err = comm_grp_ranks_local(subc->local_comm, subc->subgrp_comm, &subc->is_root_sg,
                                   &subc->subgrp_root, &subgrp_ranks, local_root);

        /* Create subcommunicator with base ranks */
        subc->base_rank[MCA_COLL_ACOLL_L3CACHE] = subc->is_root_sg == 1 ? local_root
                                                                        : subgrp_ranks[0];
        color = local_rank == subc->base_rank[MCA_COLL_ACOLL_L3CACHE] ? 0 : 1;
        parent_comm[MCA_COLL_ACOLL_LYR_NODE] = subc->local_comm;
        parent_comm[MCA_COLL_ACOLL_LYR_SOCKET] = subc->socket_comm;
        err = mca_coll_acoll_create_base_comm(parent_comm, subc, color, local_rank,
                                              subc->local_root, MCA_COLL_ACOLL_L3CACHE);

        /* Find out local rank of root in numa comm */
        err = comm_grp_ranks_local(subc->local_comm, subc->numa_comm, &subc->is_root_numa,
                                   &subc->numa_root, &numa_ranks, local_root);

        subc->base_rank[MCA_COLL_ACOLL_NUMA] = subc->is_root_numa == 1 ? local_root : numa_ranks[0];
        color = local_rank == subc->base_rank[MCA_COLL_ACOLL_NUMA] ? 0 : 1;
        err = mca_coll_acoll_create_base_comm(parent_comm, subc, color, local_rank,
                                              subc->local_root, MCA_COLL_ACOLL_NUMA);

        if (socket_ranks != NULL) {
            free(socket_ranks);
            socket_ranks = NULL;
        }
        if (subgrp_ranks != NULL) {
            free(subgrp_ranks);
            subgrp_ranks = NULL;
        }
        if (numa_ranks != NULL) {
            free(numa_ranks);
            numa_ranks = NULL;
        }
    } else {
        /* Intra node case */
        int color;
        int is_root_socket = 0;
        int *subgrp_ranks = NULL, *numa_ranks = NULL, *socket_ranks = NULL;
        ompi_communicator_t *parent_comm[MCA_COLL_ACOLL_NUM_LAYERS];

        /* Initializations */
        subc->local_root[MCA_COLL_ACOLL_LYR_NODE] = root;
        subc->local_root[MCA_COLL_ACOLL_LYR_SOCKET] = 0;

        /* Find out local rank of root in socket comm */
        err = comm_grp_ranks_local(comm, subc->socket_comm, &subc->is_root_socket,
                                   &subc->local_root[MCA_COLL_ACOLL_LYR_SOCKET], &socket_ranks,
                                   root);

        /* Create subcommunicator with socket leaders */
        subc->socket_rank = subc->is_root_socket == 1 ? root : socket_ranks[0];
        color = rank == subc->socket_rank ? 0 : 1;
        err = ompi_comm_split(comm, color, rank, &subc->socket_ldr_comm, false);
        if (MPI_SUCCESS != err) {
            return err;
        }

        /* Find out local rank of root in socket leader comm */
        err = comm_grp_ranks_local(comm, subc->socket_ldr_comm, &is_root_socket,
                                   &subc->socket_ldr_root, NULL, root);

        /* Find out local rank of root in subgroup comm */
        err = comm_grp_ranks_local(comm, subc->subgrp_comm, &subc->is_root_sg, &subc->subgrp_root,
                                   &subgrp_ranks, root);

        /* Create subcommunicator with base ranks */
        subc->base_rank[MCA_COLL_ACOLL_L3CACHE] = subc->is_root_sg == 1 ? root : subgrp_ranks[0];
        color = rank == subc->base_rank[MCA_COLL_ACOLL_L3CACHE] ? 0 : 1;
        parent_comm[MCA_COLL_ACOLL_LYR_NODE] = subc->local_comm;
        parent_comm[MCA_COLL_ACOLL_LYR_SOCKET] = subc->socket_comm;
        err = mca_coll_acoll_create_base_comm(parent_comm, subc, color, rank, subc->local_root,
                                              MCA_COLL_ACOLL_L3CACHE);

        int numa_rank;
        numa_rank = ompi_comm_rank(subc->numa_comm);
        color = (numa_rank == 0) ? 0 : 1;
        err = ompi_comm_split(subc->local_comm, color, rank, &subc->numa_comm_ldrs, false);

        /* Find out local rank of root in numa comm */
        err = comm_grp_ranks_local(comm, subc->numa_comm, &subc->is_root_numa, &subc->numa_root,
                                   &numa_ranks, root);

        subc->base_rank[MCA_COLL_ACOLL_NUMA] = subc->is_root_numa == 1 ? root : numa_ranks[0];
        color = rank == subc->base_rank[MCA_COLL_ACOLL_NUMA] ? 0 : 1;
        err = mca_coll_acoll_create_base_comm(parent_comm, subc, color, rank, subc->local_root,
                                              MCA_COLL_ACOLL_NUMA);

        if (socket_ranks != NULL) {
            free(socket_ranks);
            socket_ranks = NULL;
        }
        if (subgrp_ranks != NULL) {
            free(subgrp_ranks);
            subgrp_ranks = NULL;
        }
        if (numa_ranks != NULL) {
            free(numa_ranks);
            numa_ranks = NULL;
        }
    }

    /* Restore originals for local and socket comms */
    (subc->local_comm)->c_coll->coll_allreduce = coll_allreduce_loc;
    (subc->local_comm)->c_coll->coll_allgather = coll_allgather_loc;
    (subc->local_comm)->c_coll->coll_bcast = coll_bcast_loc;
    (subc->socket_comm)->c_coll->coll_allreduce = coll_allreduce_soc;
    (subc->socket_comm)->c_coll->coll_allgather = coll_allgather_soc;
    (subc->socket_comm)->c_coll->coll_bcast = coll_bcast_soc;

    /* For collectives where order is important (like gather, allgather),
     * split based on ranks. This is optimal for global communicators with
     * equal split among nodes, but suboptimal for other cases.
     */
    if (!subc->initialized) {
        if (subc->num_nodes > 1) {
            int node_size = (size + subc->num_nodes - 1) / subc->num_nodes;
            int color = rank / node_size;
            err = ompi_comm_split(comm, color, rank, &subc->local_r_comm, false);
            if (MPI_SUCCESS != err) {
                return err;
            }
        }
        subc->derived_node_size = (size + subc->num_nodes - 1) / subc->num_nodes;
    }

    /* Restore originals */
    (comm)->c_coll->coll_allreduce = coll_allreduce_org;
    (comm)->c_coll->coll_allgather = coll_allgather_org;
    (comm)->c_coll->coll_bcast = coll_bcast_org;

    /* Init done */
    subc->initialized = 1;
    if (root != subc->prev_init_root) {
        subc->num_root_change++;
    }
    subc->prev_init_root = root;

    return err;
}

#ifdef HAVE_XPMEM_H
static inline int mca_coll_acoll_xpmem_register(void *xpmem_apid, void *base, size_t size,
                                                mca_rcache_base_registration_t *reg)
{
    struct xpmem_addr xpmem_addr;
    xpmem_addr.apid = *((xpmem_apid_t *) xpmem_apid);
    xpmem_addr.offset = (uintptr_t) base;
    struct acoll_xpmem_rcache_reg_t *xpmem_reg = (struct acoll_xpmem_rcache_reg_t *) reg;
    xpmem_reg->xpmem_vaddr = xpmem_attach(xpmem_addr, size, NULL);

    if ((void *) -1 == xpmem_reg->xpmem_vaddr) {
        return -1;
    }
    return 0;
}

static inline int mca_coll_acoll_xpmem_deregister(void *xpmem_apid,
                                                  mca_rcache_base_registration_t *reg)
{
    int status = xpmem_detach(((struct acoll_xpmem_rcache_reg_t *) reg)->xpmem_vaddr);
    return status;
}
#endif

static inline int coll_acoll_init(mca_coll_base_module_t *module, ompi_communicator_t *comm,
                                  coll_acoll_data_t *data)
{
    int size, ret = 0, rank, line;

    mca_coll_acoll_module_t *acoll_module = (mca_coll_acoll_module_t *) module;
    coll_acoll_subcomms_t *subc;
    int cid = ompi_comm_get_local_cid(comm);
    subc = &acoll_module->subc[cid];
    if (subc->initialized_data) {
        return ret;
    }
    subc->cid = cid;
    data = (coll_acoll_data_t *) malloc(sizeof(coll_acoll_data_t));
    if (NULL == data) {
        line = __LINE__;
        ret = OMPI_ERR_OUT_OF_RESOURCE;
        goto error_hndl;
    }
    size = ompi_comm_size(comm);
    rank = ompi_comm_rank(comm);
    data->comm_size = size;

#ifdef HAVE_XPMEM_H
    if (subc->xpmem_use_sr_buf == 0) {
        data->scratch = (char *) malloc(subc->xpmem_buf_size);
        if (NULL == data->scratch) {
            line = __LINE__;
            ret = OMPI_ERR_OUT_OF_RESOURCE;
            goto error_hndl;
        }
    } else {
        data->scratch = NULL;
    }

    xpmem_segid_t seg_id;
    data->allseg_id = (xpmem_segid_t *) malloc(sizeof(xpmem_segid_t) * size);
    if (NULL == data->allseg_id) {
        line = __LINE__;
        ret = OMPI_ERR_OUT_OF_RESOURCE;
        goto error_hndl;
    }
    data->all_apid = (xpmem_apid_t *) malloc(sizeof(xpmem_apid_t) * size);
    if (NULL == data->all_apid) {
        line = __LINE__;
        ret = OMPI_ERR_OUT_OF_RESOURCE;
        goto error_hndl;
    }
    data->allshm_sbuf = (void **) malloc(sizeof(void *) * size);
    if (NULL == data->allshm_sbuf) {
        line = __LINE__;
        ret = OMPI_ERR_OUT_OF_RESOURCE;
        goto error_hndl;
    }
    data->allshm_rbuf = (void **) malloc(sizeof(void *) * size);
    if (NULL == data->allshm_rbuf) {
        line = __LINE__;
        ret = OMPI_ERR_OUT_OF_RESOURCE;
        goto error_hndl;
    }
    data->xpmem_saddr = (void **) malloc(sizeof(void *) * size);
    if (NULL == data->xpmem_saddr) {
        line = __LINE__;
        ret = OMPI_ERR_OUT_OF_RESOURCE;
        goto error_hndl;
    }
    data->xpmem_raddr = (void **) malloc(sizeof(void *) * size);
    if (NULL == data->xpmem_raddr) {
        line = __LINE__;
        ret = OMPI_ERR_OUT_OF_RESOURCE;
        goto error_hndl;
    }
    data->rcache = (mca_rcache_base_module_t **) malloc(sizeof(mca_rcache_base_module_t *) * size);
    if (NULL == data->rcache) {
        line = __LINE__;
        ret = OMPI_ERR_OUT_OF_RESOURCE;
        goto error_hndl;
    }
    seg_id = xpmem_make(0, XPMEM_MAXADDR_SIZE, XPMEM_PERMIT_MODE, (void *) 0666);
    if (seg_id == -1) {
        line = __LINE__;
        ret = -1;
        goto error_hndl;
    }

    ret = comm->c_coll->coll_allgather(&seg_id, sizeof(xpmem_segid_t), MPI_BYTE, data->allseg_id,
                                       sizeof(xpmem_segid_t), MPI_BYTE, comm,
                                       comm->c_coll->coll_allgather_module);

    /* Assuming the length of rcache name is less than 50 characters */
    char rc_name[50];
    for (int i = 0; i < size; i++) {
        if (rank != i) {
            data->all_apid[i] = xpmem_get(data->allseg_id[i], XPMEM_RDWR, XPMEM_PERMIT_MODE,
                                          (void *) 0666);
            if (data->all_apid[i] == -1) {
                line = __LINE__;
                ret = -1;
                goto error_hndl;
            }
            if (data->all_apid[i] == -1) {
                printf("Error in xpmem_get %llu, %d\n", data->all_apid[i], rank);
                line = __LINE__;
                ret = -1;
                goto error_hndl;
            }
            sprintf(rc_name, "acoll_%d_%d_%d", cid, rank, i);
            mca_rcache_base_resources_t rcache_element
                = {.cache_name = rc_name,
                   .reg_data = &data->all_apid[i],
                   .sizeof_reg = sizeof(struct acoll_xpmem_rcache_reg_t),
                   .register_mem = mca_coll_acoll_xpmem_register,
                   .deregister_mem = mca_coll_acoll_xpmem_deregister};

            data->rcache[i] = mca_rcache_base_module_create("grdma", NULL, &rcache_element);
            if (data->rcache[i] == NULL) {
                printf("Error in rcache create\n");
                ret = -1;
                line = __LINE__;
                goto error_hndl;
            }
        }
    }
#endif

    /* temporary variables */
    int tmp1, tmp2, tmp3 = 0;
    comm_grp_ranks_local(comm, subc->numa_comm, &tmp1, &tmp2, &data->l1_gp, tmp3);
    data->l1_gp_size = ompi_comm_size(subc->numa_comm);
    data->l1_local_rank = ompi_comm_rank(subc->numa_comm);

    comm_grp_ranks_local(comm, subc->numa_comm_ldrs, &tmp1, &tmp2, &data->l2_gp, tmp3);
    data->l2_gp_size = ompi_comm_size(subc->numa_comm_ldrs);
    data->l2_local_rank = ompi_comm_rank(subc->numa_comm_ldrs);
    data->offset[0] = 16 * 1024;
    data->offset[1] = data->offset[0] + size * 64;
    data->offset[2] = data->offset[1] + size * 64;
    data->offset[3] = data->offset[2] + rank * 8 * 1024;
    data->allshmseg_id = (opal_shmem_ds_t *) malloc(sizeof(opal_shmem_ds_t) * size);
    data->allshmmmap_sbuf = (void **) malloc(sizeof(void *) * size);
    data->sync[0] = 0;
    data->sync[1] = 0;
    char *shfn;

    /* Only the leaders need to allocate shared memory */
    /* remaining ranks move their data into their leader's shm */
    if (data->l1_gp[0] == rank) {
        subc->initialized_shm_data = true;
        ret = asprintf(&shfn, "/dev/shm/acoll_coll_shmem_seg.%u.%x.%d:%d-%d", geteuid(),
                       OPAL_PROC_MY_NAME.jobid, ompi_comm_rank(MPI_COMM_WORLD),
                       ompi_comm_get_local_cid(comm), ompi_comm_size(comm));
    }

    if (ret < 0) {
        line = __LINE__;
        goto error_hndl;
    }

    opal_shmem_ds_t seg_ds;
    if (data->l1_gp[0] == rank) {
        /* Assuming cacheline size is 64 */
        long memsize
            = (16 * 1024 /* scratch leader */ + 64 * size /* sync variables l1 group*/
               + 64 * size /* sync variables l2 group*/ + 8 * 1024 * size /*data from ranks*/);
        ret = opal_shmem_segment_create(&seg_ds, shfn, memsize);
        free(shfn);
    }

    if (ret != OPAL_SUCCESS) {
        opal_output_verbose(MCA_BASE_VERBOSE_ERROR, ompi_coll_base_framework.framework_output,
                            "coll:acoll: Error: Could not create shared memory segment");
        line = __LINE__;
        goto error_hndl;
    }

    ret = comm->c_coll->coll_allgather(&seg_ds, sizeof(opal_shmem_ds_t), MPI_BYTE,
                                       data->allshmseg_id, sizeof(opal_shmem_ds_t), MPI_BYTE, comm,
                                       comm->c_coll->coll_allgather_module);

    if (data->l1_gp[0] != rank) {
        data->allshmmmap_sbuf[data->l1_gp[0]] = opal_shmem_segment_attach(
            &data->allshmseg_id[data->l1_gp[0]]);
    } else {
        for (int i = 0; i < data->l2_gp_size; i++) {
            data->allshmmmap_sbuf[data->l2_gp[i]] = opal_shmem_segment_attach(
                &data->allshmseg_id[data->l2_gp[i]]);
        }
    }

    int offset = 16 * 1024;
    memset(((char *) data->allshmmmap_sbuf[data->l1_gp[0]]) + offset + 64 * rank, 0, 64);
    if (data->l1_gp[0] == rank) {
        memset(((char *) data->allshmmmap_sbuf[data->l2_gp[0]]) + (offset + 64 * size) + 64 * rank,
               0, 64);
    }

    subc->initialized_data = true;
    subc->data = data;
    ompi_coll_base_barrier_intra_tree(comm, module);

    return MPI_SUCCESS;
error_hndl:
    (void) line;
    if (NULL != data) {
#ifdef HAVE_XPMEM_H
        free(data->allseg_id);
        data->allseg_id = NULL;
        free(data->all_apid);
        data->all_apid = NULL;
        free(data->allshm_sbuf);
        data->allshm_sbuf = NULL;
        free(data->allshm_rbuf);
        data->allshm_rbuf = NULL;
        free(data->xpmem_saddr);
        data->xpmem_saddr = NULL;
        free(data->xpmem_raddr);
        data->xpmem_raddr = NULL;
        free(data->rcache);
        data->rcache = NULL;
        free(data->scratch);
        data->scratch = NULL;
#endif
        free(data->allshmseg_id);
        data->allshmseg_id = NULL;
        free(data->allshmmmap_sbuf);
        data->allshmmmap_sbuf = NULL;
        free(data->l1_gp);
        data->l1_gp = NULL;
        free(data->l2_gp);
        data->l2_gp = NULL;
        free(data);
        data = NULL;
    }
    return ret;
}

#ifdef HAVE_XPMEM_H
static inline void register_and_cache(int size, size_t total_dsize, int rank,
                                      coll_acoll_data_t *data)
{
    uintptr_t base, bound;
    for (int i = 0; i < size; i++) {
        if (rank != i) {
            mca_rcache_base_module_t *rcache_i = data->rcache[i];
            int access_flags = 0;
            struct acoll_xpmem_rcache_reg_t *sbuf_reg = NULL, *rbuf_reg = NULL;
            base = OPAL_DOWN_ALIGN((uintptr_t) data->allshm_sbuf[i], 4096, uintptr_t);
            bound = OPAL_ALIGN((uintptr_t) data->allshm_sbuf[i] + total_dsize, 4096, uintptr_t);
            int ret = rcache_i->rcache_register(rcache_i, (void *) base, bound - base, access_flags,
                                                MCA_RCACHE_ACCESS_ANY,
                                                (mca_rcache_base_registration_t **) &sbuf_reg);

            if (ret != 0) {
                sbuf_reg = NULL;
                return;
            }
            data->xpmem_saddr[i] = (void *) ((uintptr_t) sbuf_reg->xpmem_vaddr
                                             + ((uintptr_t) data->allshm_sbuf[i]
                                                - (uintptr_t) sbuf_reg->base.base));

            base = OPAL_DOWN_ALIGN((uintptr_t) data->allshm_rbuf[i], 4096, uintptr_t);
            bound = OPAL_ALIGN((uintptr_t) data->allshm_rbuf[i] + total_dsize, 4096, uintptr_t);
            ret = rcache_i->rcache_register(rcache_i, (void *) base, bound - base, access_flags,
                                            MCA_RCACHE_ACCESS_ANY,
                                            (mca_rcache_base_registration_t **) &rbuf_reg);

            if (ret != 0) {
                rbuf_reg = NULL;
                return;
            }
            data->xpmem_raddr[i] = (void *) ((uintptr_t) rbuf_reg->xpmem_vaddr
                                             + ((uintptr_t) data->allshm_rbuf[i]
                                                - (uintptr_t) rbuf_reg->base.base));
        } else {
            data->xpmem_saddr[i] = data->allshm_sbuf[i];
            data->xpmem_raddr[i] = data->allshm_rbuf[i];
        }
    }
}
#endif
