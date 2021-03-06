/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2014 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006      Los Alamos National Security, LLC.  All rights
 *                         reserved. 
 *
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "ompi_config.h"
#include <string.h>
#include "opal/class/opal_bitmap.h"
#include "ompi/mca/btl/btl.h"

#include "btl_tcp.h"
#include "btl_tcp_frag.h" 
#include "btl_tcp_proc.h"
#include "opal/datatype/opal_convertor.h" 
#include "ompi/mca/mpool/base/base.h" 
#include "ompi/mca/mpool/mpool.h" 
#include "ompi/proc/proc.h"
#include "opal/mca/db/db.h"


#define STR_FROM_ENDPOINT(x) inet_ntoa(x->endpoint_addr->addr_inet._union_inet._addr__inet._addr_inet)


mca_btl_tcp_module_t mca_btl_tcp_module = {
    {
        &mca_btl_tcp_component.super,
        0, /* max size of first fragment */
        0, /* min send fragment size */
        0, /* max send fragment size */
        0, /* btl_rdma_pipeline_send_length */
        0, /* btl_rdma_pipeline_frag_size */
        0, /* btl_min_rdma_pipeline_size */
        0, /* exclusivity */
        0, /* latency */
        0, /* bandwidth */
        0, /* flags */
        0, /* segment size */
        mca_btl_tcp_add_procs,
        mca_btl_tcp_del_procs,
        NULL, 
        mca_btl_tcp_finalize,
        mca_btl_tcp_alloc, 
        mca_btl_tcp_free, 
        mca_btl_tcp_prepare_src,
        mca_btl_tcp_prepare_dst,
        mca_btl_tcp_send,
        NULL, /* send immediate */
        mca_btl_tcp_put,
        NULL, /* get */ 
        mca_btl_base_dump,
        NULL, /* mpool */
        NULL, /* register error */
        mca_btl_tcp_ft_event,
#if ORTE_ENABLE_MIGRATION
        mca_btl_tcp_mig_event
#endif
    }
};

#if ORTE_ENABLE_MIGRATION

int migration_state = BTL_RUNNING;

bool is_ep_migrating(mca_btl_base_endpoint_t *endpoint) {
    int ret;
    ompi_vpid_t *vptr, vpid;

    vptr = &vpid;

    // Get the id of the process that is the endpoint
    if (OMPI_SUCCESS != (ret = opal_db.fetch((opal_identifier_t*)
        &endpoint->endpoint_proc->proc_ompi->proc_name, OMPI_RTE_NODE_ID, (void**)&vptr, OPAL_UINT32))) {
        opal_output_verbose(0, ompi_btl_base_framework.framework_output,
            "%s btl:tcp: vpid for migration not found in the local database.",
            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        return false;
    }

    return btl_mig_src_vpid == vpid;
}

/**
 * Restore frozen endpoints after migration. If a frag has been appended before migration, start a new connection in order to send it.
 * @param btl This btl_tcp module
 * @return 
 */
static int mca_btl_tcp_mig_restore(mca_btl_base_module_t* btl){
    mca_btl_base_endpoint_t *endpoint, *next;
    mca_btl_tcp_module_t *tcp_btl = (mca_btl_tcp_module_t *)btl;
    
    opal_output_verbose(0, ompi_btl_base_framework.framework_output,
        "%s btl:tcp: restoring endpoints...",
        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
    
    switch(migration_state){
        case BTL_MIGRATING_DONE:
            OPAL_LIST_FOREACH_SAFE(endpoint, next, &tcp_btl->tcp_endpoints, mca_btl_base_endpoint_t) {
                if(MCA_BTL_TCP_FROZEN == endpoint->endpoint_state) {
                    endpoint->endpoint_state = MCA_BTL_TCP_CLOSED;
                    if(opal_list_get_size(&endpoint->endpoint_frags) > 0 || endpoint->endpoint_send_frag != NULL) {
                        mca_btl_tcp_endpoint_start_connect(endpoint);
                    }
                }
            }
            break;
        case BTL_NOT_MIGRATING_DONE:
            OPAL_LIST_FOREACH_SAFE(endpoint, next, &tcp_btl->tcp_endpoints, mca_btl_base_endpoint_t) {
                if(MCA_BTL_TCP_FROZEN == endpoint->endpoint_state){
                    endpoint->endpoint_state = MCA_BTL_TCP_CLOSED;
                    if(opal_list_get_size(&endpoint->endpoint_frags) > 0 || endpoint->endpoint_send_frag != NULL) {
                        mca_btl_tcp_endpoint_start_connect(endpoint);
                    }
                }
            }
            break;
        default:
            opal_output_verbose(0, ompi_btl_base_framework.framework_output,
                "%s btl:tcp: unexpected migration state",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
    }
    
    opal_output_verbose(0, ompi_btl_base_framework.framework_output,
        "%s btl:tcp: done restoring endpoints.",
        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
    
    return OMPI_SUCCESS;
}

/**
 * Close sockets of the endpoints involved in the ongoing migration, stop events and update migrating host address
 * @param btl This btl_tcp module
 */
static void mca_btl_tcp_mig_close_sockets(mca_btl_base_module_t* btl){
    mca_btl_base_endpoint_t *endpoint, *next;
    mca_btl_tcp_module_t *tcp_btl = (mca_btl_tcp_module_t *)btl;
    
    struct in_addr address;
    const char *hostname = strchr(btl_mig_dst, '@')+1;
    inet_aton(hostname, &address);

    opal_output_verbose(0, ompi_btl_base_framework.framework_output,
        "%s btl:tcp: closing sockets...",
        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
    
        switch(migration_state){
            case BTL_MIGRATING_EXEC:
                OPAL_LIST_FOREACH_SAFE(endpoint, next, &tcp_btl->tcp_endpoints, mca_btl_base_endpoint_t) {

                    if (
                       MCA_BTL_TCP_CLOSED != endpoint->endpoint_state &&
                       MCA_BTL_TCP_FAILED != endpoint->endpoint_state    ) {

                        if(endpoint->endpoint_sd > 0) {
	                        if(0 != close(endpoint->endpoint_sd)) {
                                opal_output_verbose(0, ompi_btl_base_framework.framework_output,
                                    "%s btl:tcp: error while closing socket on %s sock %i errno %i",
                                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),endpoint->endpoint_proc->proc_ompi->proc_hostname,
         				endpoint->endpoint_sd, errno);
                                }
			}
                        if (endpoint->endpoint_recv_event.ev_base != NULL)
                            opal_event_del(&endpoint->endpoint_recv_event);
                        if (endpoint->endpoint_send_event.ev_base != NULL)
                            opal_event_del(&endpoint->endpoint_send_event);
                        endpoint->endpoint_state = MCA_BTL_TCP_FROZEN;
                        endpoint->endpoint_sd = -1;
                    }

                    if( is_ep_migrating(endpoint) ){
                        endpoint->endpoint_addr->addr_inet._union_inet._addr__inet._addr_inet = address;
                        strcpy(endpoint->endpoint_proc->proc_ompi->proc_hostname, btl_mig_dst);
                    }
                }
                break;
            case BTL_NOT_MIGRATING_EXEC:
                OPAL_LIST_FOREACH_SAFE(endpoint, next, &tcp_btl->tcp_endpoints, mca_btl_base_endpoint_t) {

                    if(is_ep_migrating(endpoint) || endpoint->endpoint_state == MCA_BTL_TCP_FROZEN) {
                        if(MCA_BTL_TCP_CLOSED != endpoint->endpoint_state && MCA_BTL_TCP_FAILED != endpoint->endpoint_state) {
                            if(endpoint->endpoint_sd > 0) {
                                if(0 != close(endpoint->endpoint_sd)){
                                    opal_output_verbose(0, ompi_btl_base_framework.framework_output,
                                        "%s btl:tcp: error while closing socket on %s",
                                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),endpoint->endpoint_proc->proc_ompi->proc_hostname);
                                }
                            }
                            if (endpoint->endpoint_recv_event.ev_base != NULL)
                                opal_event_del(&endpoint->endpoint_recv_event);
                            if (endpoint->endpoint_send_event.ev_base != NULL)
                                opal_event_del(&endpoint->endpoint_send_event);
                            endpoint->endpoint_state = MCA_BTL_TCP_FROZEN;
                            endpoint->endpoint_sd = -1;
                        }
                        endpoint->endpoint_addr->addr_inet._union_inet._addr__inet._addr_inet = address;
                        strcpy(endpoint->endpoint_proc->proc_ompi->proc_hostname, btl_mig_dst);
                    }
                }
                break;
            default:
                opal_output_verbose(0, ompi_btl_base_framework.framework_output,
                                "%s btl:tcp: unexpected migration state",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                ;
        }
        opal_output_verbose(0, ompi_btl_base_framework.framework_output,
            "%s btl:tcp: done closing sockets.",
            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));

}

/**
 * Freeze active endpoints in order to stop them from sending data, migration is about to start.
 * @param btl This btl_tcp module
 */

static void mca_btl_tcp_freeze_endpoints(mca_btl_base_module_t* btl){
    mca_btl_base_endpoint_t *endpoint, *next;
    mca_btl_tcp_module_t *tcp_btl = (mca_btl_tcp_module_t *)btl;
    
    opal_output_verbose(0, ompi_btl_base_framework.framework_output,
                    "%s btl:tcp: freezing endpoints...",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
    
    switch(migration_state){
        //If I'm the migrating node I have to freeze all my endpoints
        case BTL_MIGRATING_PREPARE:
            OPAL_LIST_FOREACH_SAFE(endpoint, next, &tcp_btl->tcp_endpoints, mca_btl_base_endpoint_t) {
                if(MCA_BTL_TCP_CLOSED != endpoint->endpoint_state && MCA_BTL_TCP_FAILED != endpoint->endpoint_state)
                    endpoint->endpoint_state = MCA_BTL_TCP_FROZEN;
                    shutdown(endpoint->endpoint_sd, SHUT_WR);
                    mca_btl_tcp_endpoint_set_blocking(endpoint, true);
            }
            break;
        //Otherwise I have to close just the endpoints directed to the migrating node
        case BTL_NOT_MIGRATING_PREPARE:
            OPAL_LIST_FOREACH_SAFE(endpoint, next, &tcp_btl->tcp_endpoints, mca_btl_base_endpoint_t) {
                if (MCA_BTL_TCP_CLOSED != endpoint->endpoint_state && MCA_BTL_TCP_FAILED != endpoint->endpoint_state) {
                    if(is_ep_migrating(endpoint)){
                        endpoint->endpoint_state = MCA_BTL_TCP_FROZEN;
                        shutdown(endpoint->endpoint_sd, SHUT_WR);
                        mca_btl_tcp_endpoint_set_blocking(endpoint, true);
                    }
                }
            }
            break;
        default:
            opal_output_verbose(0, ompi_btl_base_framework.framework_output,
                "%s btl:tcp: unexpected migration state.",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
    }
    
    opal_output_verbose(0, ompi_btl_base_framework.framework_output,
                "%s btl:tcp: done freezing endpoints.",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
}

/**
 * FSM which manages migration state. Triggered by btl_base_frame each time it gets signaled.
 * @param event New migration state
 * @param data  Data passed by btl_base_frame. It's basically this btl_tcp module
 * @return
 */
int mca_btl_tcp_mig_event(int event, void *data){    
    migration_state = event;

    switch(migration_state){
        case BTL_MIGRATING_PREPARE:
        case BTL_NOT_MIGRATING_PREPARE:
            mca_btl_tcp_freeze_endpoints((mca_btl_base_module_t *) data);

            break;
        case BTL_MIGRATING_EXEC:
        case BTL_NOT_MIGRATING_EXEC:
            // Run an event loop: if a socket is pending for reading or writing
            // we must ensure to manage it (in case of migrating node the call is
            // blocking)
            opal_event_loop(opal_event_base, EVLOOP_ONCE | EVLOOP_NONBLOCK);

            mca_btl_tcp_mig_close_sockets((mca_btl_base_module_t *) data);
            break;
        case BTL_NOT_MIGRATING_DONE:
        case BTL_MIGRATING_DONE:
            mca_btl_tcp_mig_restore((mca_btl_base_module_t *) data);

            // In this case we can reset the migration status variable
            // to running (the migration has ended)
            migration_state = BTL_RUNNING;
            break;
        default:
            break;
    }
    return OMPI_SUCCESS;
}
#endif

/**
 *
 */

int mca_btl_tcp_add_procs( struct mca_btl_base_module_t* btl, 
                           size_t nprocs, 
                           struct ompi_proc_t **ompi_procs, 
                           struct mca_btl_base_endpoint_t** peers, 
                           opal_bitmap_t* reachable )
{
    mca_btl_tcp_module_t* tcp_btl = (mca_btl_tcp_module_t*)btl;
    ompi_proc_t* my_proc; /* pointer to caller's proc structure */
    int i, rc;

    /* get pointer to my proc structure */
    my_proc = ompi_proc_local();
    if( NULL == my_proc ) {
        return OMPI_ERR_OUT_OF_RESOURCE;
    }
    for(i = 0; i < (int) nprocs; i++) {

        struct ompi_proc_t* ompi_proc = ompi_procs[i];
        mca_btl_tcp_proc_t* tcp_proc;
        mca_btl_base_endpoint_t* tcp_endpoint;

        /* Do not create loopback TCP connections */
        if( my_proc == ompi_proc ) {
            continue;
        }

        if(NULL == (tcp_proc = mca_btl_tcp_proc_create(ompi_proc))) {
            continue;
        }

        /*
         * Check to make sure that the peer has at least as many interface 
         * addresses exported as we are trying to use. If not, then 
         * don't bind this BTL instance to the proc.
         */

        OPAL_THREAD_LOCK(&tcp_proc->proc_lock);

        /* The btl_proc datastructure is shared by all TCP BTL
         * instances that are trying to reach this destination. 
         * Cache the peer instance on the btl_proc.
         */
        tcp_endpoint = OBJ_NEW(mca_btl_tcp_endpoint_t);
        if(NULL == tcp_endpoint) {
            OPAL_THREAD_UNLOCK(&tcp_proc->proc_lock);
            OBJ_RELEASE(ompi_proc);
            return OMPI_ERR_OUT_OF_RESOURCE;
        }

        tcp_endpoint->endpoint_btl = tcp_btl;
        rc = mca_btl_tcp_proc_insert(tcp_proc, tcp_endpoint);
        if(rc != OMPI_SUCCESS) {
            OPAL_THREAD_UNLOCK(&tcp_proc->proc_lock);
            OBJ_RELEASE(ompi_proc);
            OBJ_RELEASE(tcp_endpoint);
            continue;
        }

        opal_bitmap_set_bit(reachable, i);
        OPAL_THREAD_UNLOCK(&tcp_proc->proc_lock);
        peers[i] = tcp_endpoint;
        opal_list_append(&tcp_btl->tcp_endpoints, (opal_list_item_t*)tcp_endpoint);

        /* we increase the count of MPI users of the event library
           once per peer, so that we are used until we aren't
           connected to a peer */
        opal_progress_event_users_increment();
    }

    return OMPI_SUCCESS;
}

int mca_btl_tcp_del_procs(struct mca_btl_base_module_t* btl, 
        size_t nprocs, 
        struct ompi_proc_t **procs, 
        struct mca_btl_base_endpoint_t ** endpoints)
{
    mca_btl_tcp_module_t* tcp_btl = (mca_btl_tcp_module_t*)btl;
    size_t i;
    for(i=0; i<nprocs; i++) {
        mca_btl_tcp_endpoint_t* tcp_endpoint = endpoints[i];
        if(tcp_endpoint->endpoint_proc != mca_btl_tcp_proc_local()) {
            opal_list_remove_item(&tcp_btl->tcp_endpoints, (opal_list_item_t*)tcp_endpoint);
            OBJ_RELEASE(tcp_endpoint);
        }
        opal_progress_event_users_decrement();
    }
    return OMPI_SUCCESS;
}


/**
 * Allocate a segment.
 *
 * @param btl (IN)      BTL module
 * @param size (IN)     Request segment size.
 */

mca_btl_base_descriptor_t* mca_btl_tcp_alloc(
    struct mca_btl_base_module_t* btl,
    struct mca_btl_base_endpoint_t* endpoint,
    uint8_t order,
    size_t size,
    uint32_t flags)
{
    mca_btl_tcp_frag_t* frag = NULL;
    
    if(size <= btl->btl_eager_limit) { 
        MCA_BTL_TCP_FRAG_ALLOC_EAGER(frag); 
    } else if (size <= btl->btl_max_send_size) { 
        MCA_BTL_TCP_FRAG_ALLOC_MAX(frag); 
    }
    if( OPAL_UNLIKELY(NULL == frag) ) {
        return NULL;
    }
    
    frag->segments[0].seg_len = size;
    frag->segments[0].seg_addr.pval = frag+1;

    frag->base.des_src = frag->segments;
    frag->base.des_src_cnt = 1;
    frag->base.des_dst = NULL;
    frag->base.des_dst_cnt = 0;
    frag->base.des_flags = flags; 
    frag->base.order = MCA_BTL_NO_ORDER;
    frag->btl = (mca_btl_tcp_module_t*)btl;
    return (mca_btl_base_descriptor_t*)frag;
}


/**
 * Return a segment
 */

int mca_btl_tcp_free(
    struct mca_btl_base_module_t* btl, 
    mca_btl_base_descriptor_t* des) 
{
    mca_btl_tcp_frag_t* frag = (mca_btl_tcp_frag_t*)des; 
    MCA_BTL_TCP_FRAG_RETURN(frag); 
    return OMPI_SUCCESS; 
}

/**
 * Pack data and return a descriptor that can be
 * used for send/put.
 *
 * @param btl (IN)      BTL module
 * @param peer (IN)     BTL peer addressing
 */
mca_btl_base_descriptor_t* mca_btl_tcp_prepare_src(
    struct mca_btl_base_module_t* btl,
    struct mca_btl_base_endpoint_t* endpoint,
    struct mca_mpool_base_registration_t* registration,
    struct opal_convertor_t* convertor,
    uint8_t order,
    size_t reserve,
    size_t* size,
    uint32_t flags)
{
    mca_btl_tcp_frag_t* frag;
    struct iovec iov;
    uint32_t iov_count = 1;
    size_t max_data = *size;
    int rc;

    if( OPAL_UNLIKELY(max_data > UINT32_MAX) ) {  /* limit the size to what we support */
        max_data = (size_t)UINT32_MAX;
    }
    /*
     * if we aren't pinning the data and the requested size is less
     * than the eager limit pack into a fragment from the eager pool
     */
    if (max_data+reserve <= btl->btl_eager_limit) {
        MCA_BTL_TCP_FRAG_ALLOC_EAGER(frag);
    } else {
        /* 
         * otherwise pack as much data as we can into a fragment
         * that is the max send size.
         */
        MCA_BTL_TCP_FRAG_ALLOC_MAX(frag);
    }
    if( OPAL_UNLIKELY(NULL == frag) ) {
        return NULL;
    }

    frag->segments[0].seg_addr.pval = (frag + 1);
    frag->segments[0].seg_len = reserve;

    frag->base.des_src_cnt = 1;
    if(opal_convertor_need_buffers(convertor)) {

        if (max_data + reserve > frag->size) {
            max_data = frag->size - reserve;
        }
        iov.iov_len = max_data;
        iov.iov_base = (IOVBASE_TYPE*)(((unsigned char*)(frag->segments[0].seg_addr.pval)) + reserve);
        
        rc = opal_convertor_pack(convertor, &iov, &iov_count, &max_data );
        if( OPAL_UNLIKELY(rc < 0) ) {
            mca_btl_tcp_free(btl, &frag->base);
            return NULL;
        }
        
        frag->segments[0].seg_len += max_data;

    } else {

        iov.iov_len = max_data;
        iov.iov_base = NULL;

        rc = opal_convertor_pack(convertor, &iov, &iov_count, &max_data );
        if( OPAL_UNLIKELY(rc < 0) ) {
            mca_btl_tcp_free(btl, &frag->base);
            return NULL;
        }

        frag->segments[1].seg_addr.pval = iov.iov_base;
        frag->segments[1].seg_len = max_data;
        frag->base.des_src_cnt = 2;
    }

    frag->base.des_src = frag->segments;
    frag->base.des_dst = NULL;
    frag->base.des_dst_cnt = 0;
    frag->base.des_flags = flags;
    frag->base.order = MCA_BTL_NO_ORDER;
    *size = max_data;
    return &frag->base;
}


/**
 * Prepare a descriptor for send/rdma using the supplied
 * convertor. If the convertor references data that is contigous,
 * the descriptor may simply point to the user buffer. Otherwise,
 * this routine is responsible for allocating buffer space and
 * packing if required.
 *
 * @param btl (IN)          BTL module
 * @param endpoint (IN)     BTL peer addressing
 * @param convertor (IN)    Data type convertor
 * @param reserve (IN)      Additional bytes requested by upper layer to precede user data
 * @param size (IN/OUT)     Number of bytes to prepare (IN), number of bytes actually prepared (OUT)
 */

mca_btl_base_descriptor_t* mca_btl_tcp_prepare_dst(
    struct mca_btl_base_module_t* btl,
    struct mca_btl_base_endpoint_t* endpoint,
    struct mca_mpool_base_registration_t* registration,
    struct opal_convertor_t* convertor,
    uint8_t order,
    size_t reserve,
    size_t* size,
    uint32_t flags)
{
    mca_btl_tcp_frag_t* frag;

    if( OPAL_UNLIKELY((*size) > UINT32_MAX) ) {  /* limit the size to what we support */
        *size = (size_t)UINT32_MAX;
    }
    MCA_BTL_TCP_FRAG_ALLOC_USER(frag);
    if( OPAL_UNLIKELY(NULL == frag) ) {
        return NULL;
    }

    frag->segments->seg_len = *size;
    opal_convertor_get_current_pointer( convertor, (void**)&(frag->segments->seg_addr.pval) );

    frag->base.des_src = NULL;
    frag->base.des_src_cnt = 0;
    frag->base.des_dst = frag->segments;
    frag->base.des_dst_cnt = 1;
    frag->base.des_flags = flags;
    frag->base.order = MCA_BTL_NO_ORDER;
    return &frag->base;
}


/**
 * Initiate an asynchronous send.
 *
 * @param btl (IN)         BTL module
 * @param endpoint (IN)    BTL addressing information
 * @param descriptor (IN)  Description of the data to be transfered
 * @param tag (IN)         The tag value used to notify the peer.
 */

int mca_btl_tcp_send( struct mca_btl_base_module_t* btl,
                      struct mca_btl_base_endpoint_t* endpoint,
                      struct mca_btl_base_descriptor_t* descriptor, 
                      mca_btl_base_tag_t tag )
{
    mca_btl_tcp_module_t* tcp_btl = (mca_btl_tcp_module_t*) btl; 
    mca_btl_tcp_frag_t* frag = (mca_btl_tcp_frag_t*)descriptor; 
    int i;

    frag->btl = tcp_btl;
    frag->endpoint = endpoint;
    frag->rc = 0;
    frag->iov_idx = 0;
    frag->iov_cnt = 1;
    frag->iov_ptr = frag->iov;
    frag->iov[0].iov_base = (IOVBASE_TYPE*)&frag->hdr;
    frag->iov[0].iov_len = sizeof(frag->hdr);
    frag->hdr.size = 0;
    for( i = 0; i < (int)frag->base.des_src_cnt; i++) {
        frag->hdr.size += frag->segments[i].seg_len;
        frag->iov[i+1].iov_len = frag->segments[i].seg_len;
        frag->iov[i+1].iov_base = (IOVBASE_TYPE*)frag->segments[i].seg_addr.pval;
        frag->iov_cnt++;
    }
    frag->hdr.base.tag = tag;
    frag->hdr.type = MCA_BTL_TCP_HDR_TYPE_SEND;
    frag->hdr.count = 0;
    if (endpoint->endpoint_nbo) MCA_BTL_TCP_HDR_HTON(frag->hdr);

#if ORTE_ENABLE_MIGRATION
    switch(migration_state){
        case BTL_MIGRATING_PREPARE:
        case BTL_MIGRATING_EXEC:
            endpoint->endpoint_state = MCA_BTL_TCP_FROZEN;
            break;
        case BTL_NOT_MIGRATING_PREPARE:
        case BTL_NOT_MIGRATING_EXEC:
            if (is_ep_migrating(endpoint)){
                endpoint->endpoint_state = MCA_BTL_TCP_FROZEN;
            }
            break;
        default:
            ;
    }    
#endif
    return mca_btl_tcp_endpoint_send(endpoint,frag);
}


/**
 * Initiate an asynchronous put.
 *
 * @param btl (IN)         BTL module
 * @param endpoint (IN)    BTL addressing information
 * @param descriptor (IN)  Description of the data to be transferred
 */

int mca_btl_tcp_put( mca_btl_base_module_t* btl,
                     mca_btl_base_endpoint_t* endpoint,
                     mca_btl_base_descriptor_t* descriptor )
{
    mca_btl_tcp_module_t* tcp_btl = (mca_btl_tcp_module_t*) btl; 
    mca_btl_tcp_frag_t* frag = (mca_btl_tcp_frag_t*)descriptor; 
    int i;

    frag->btl = tcp_btl;
    frag->endpoint = endpoint;
    frag->rc = 0;
    frag->iov_idx = 0;
    frag->hdr.size = 0;
    frag->iov_cnt = 2;
    frag->iov_ptr = frag->iov;
    frag->iov[0].iov_base = (IOVBASE_TYPE*)&frag->hdr;
    frag->iov[0].iov_len = sizeof(frag->hdr);
    frag->iov[1].iov_base = (IOVBASE_TYPE*)frag->base.des_dst;
    frag->iov[1].iov_len = frag->base.des_dst_cnt * sizeof(mca_btl_base_segment_t);
    for( i = 0; i < (int)frag->base.des_src_cnt; i++ ) {
        frag->hdr.size += frag->segments[i].seg_len;
        frag->iov[i+2].iov_len = frag->segments[i].seg_len;
        frag->iov[i+2].iov_base = (IOVBASE_TYPE*)frag->segments[i].seg_addr.pval;
        frag->iov_cnt++;
    }
    frag->hdr.base.tag = MCA_BTL_TAG_BTL;
    frag->hdr.type = MCA_BTL_TCP_HDR_TYPE_PUT;
    frag->hdr.count = frag->base.des_dst_cnt;
    if (endpoint->endpoint_nbo) MCA_BTL_TCP_HDR_HTON(frag->hdr);
    
#if ORTE_ENABLE_MIGRATION
    switch(migration_state){
        case BTL_MIGRATING_PREPARE:
        case BTL_MIGRATING_EXEC:
            endpoint->endpoint_state = MCA_BTL_TCP_FROZEN;
            break;
        case BTL_NOT_MIGRATING_PREPARE:
        case BTL_NOT_MIGRATING_EXEC:
            if (is_ep_migrating(endpoint)){
                endpoint->endpoint_state = MCA_BTL_TCP_FROZEN;
            }
            break;
        default:
            ;
    }
#endif
    
    return ((i = mca_btl_tcp_endpoint_send(endpoint,frag)) >= 0 ? OMPI_SUCCESS : i);
}


/**
 * Initiate an asynchronous get.
 *
 * @param btl (IN)         BTL module
 * @param endpoint (IN)    BTL addressing information
 * @param descriptor (IN)  Description of the data to be transferred
 *
 */

int mca_btl_tcp_get( 
    mca_btl_base_module_t* btl,
    mca_btl_base_endpoint_t* endpoint,
    mca_btl_base_descriptor_t* descriptor)
{
    mca_btl_tcp_module_t* tcp_btl = (mca_btl_tcp_module_t*) btl; 
    mca_btl_tcp_frag_t* frag = (mca_btl_tcp_frag_t*)descriptor; 
    int rc;

    frag->btl = tcp_btl;
    frag->endpoint = endpoint;
    frag->rc = 0;
    frag->iov_idx = 0;
    frag->hdr.size = 0;
    frag->iov_cnt = 2;
    frag->iov_ptr = frag->iov;
    frag->iov[0].iov_base = (IOVBASE_TYPE*)&frag->hdr;
    frag->iov[0].iov_len = sizeof(frag->hdr);
    frag->iov[1].iov_base = (IOVBASE_TYPE*)frag->base.des_src;
    frag->iov[1].iov_len = frag->base.des_src_cnt * sizeof(mca_btl_base_segment_t);
    frag->hdr.base.tag = MCA_BTL_TAG_BTL;
    frag->hdr.type = MCA_BTL_TCP_HDR_TYPE_GET;
    frag->hdr.count = frag->base.des_src_cnt;
    if (endpoint->endpoint_nbo) MCA_BTL_TCP_HDR_HTON(frag->hdr);

#if ORTE_ENABLE_MIGRATION
    switch(migration_state){
        case BTL_MIGRATING_PREPARE:
        case BTL_MIGRATING_EXEC:
            endpoint->endpoint_state = MCA_BTL_TCP_FROZEN;
            break;
        case BTL_NOT_MIGRATING_PREPARE:
        case BTL_NOT_MIGRATING_EXEC:
            if (is_ep_migrating(endpoint)){
                endpoint->endpoint_state = MCA_BTL_TCP_FROZEN;
            }
            break;
        default:
            ;
    }
#endif
    return ((rc = mca_btl_tcp_endpoint_send(endpoint,frag)) >= 0 ? OMPI_SUCCESS : rc);
}


/*
 * Cleanup/release module resources.
 */

int mca_btl_tcp_finalize(struct mca_btl_base_module_t* btl)
{
    mca_btl_tcp_module_t* tcp_btl = (mca_btl_tcp_module_t*) btl; 
    opal_list_item_t* item;
    for( item = opal_list_remove_first(&tcp_btl->tcp_endpoints);
         item != NULL;
         item = opal_list_remove_first(&tcp_btl->tcp_endpoints)) {
        mca_btl_tcp_endpoint_t *endpoint = (mca_btl_tcp_endpoint_t*)item;
        OBJ_RELEASE(endpoint);
        opal_progress_event_users_decrement();
    }
    free(tcp_btl);
    return OMPI_SUCCESS;
}
