#include "ua_server_internal.h"

void processWork(UA_Server *server, const UA_WorkItem *items, UA_Int32 itemsSize) {
    for(UA_Int32 i=0;i<itemsSize;i++) {
        const UA_WorkItem *item = &items[i];
        switch(item->type) {
        case UA_WORKITEMTYPE_BINARYNETWORKMESSAGE:
            UA_Server_processBinaryMessage(server, item->item.binaryNetworkMessage.connection,
                                           &item->item.binaryNetworkMessage.message);
            UA_ByteString_deleteMembers((UA_ByteString *)&item->item.binaryNetworkMessage.message);
            break;
        case UA_WORKITEMTYPE_BINARYNETWORKCLOSED:
            // todo: internal cleanup in the server.
            if(item->item.binaryNetworkMessage.connection->channel)
                // remove the connection from the securechannel.
                item->item.binaryNetworkClose->channel->connection = UA_NULL;
            UA_free(item->item.binaryNetworkClose); // TODO!! Not so fast. Others might still use it.
            break;
        case UA_WORKITEMTYPE_METHODCALL:
            item->item.methodCall.method(server, item->item.methodCall.data);
            break;
        default:
            assert(UA_FALSE);
        }
    }
}

#ifdef MULTITHREADING
void * runWorkerLoop(UA_Server *server, UA_UInt32 **workerCounter) {
   	rcu_register_thread();
    UA_UInt32 *c = UA_alloc(sizeof(UA_UInt32));
    *workerCounter = c;
    uatomic_set(c, 0);
    while(*(server->running)) {
        struct cds_wfq_node *node = cds_wfq_dequeue_blocking(&server->dispatchQueue);
        if(!node)
            continue;
        struct _workListNode *wln = (struct _workListNode*)node;
        processWorkList(server,&wln->workList);
        UA_free((void *)wln->workList.workItems);
        UA_free(wln);
        uatomic_inc(c); // increase the workerCounter;
    }
   	rcu_unregister_thread();
    return UA_NULL;
}
#endif

/** The timed event and repeated event are only relevant for the main thread,
    not the workers. */
struct timedEvent {
    UA_WorkItem workItem;
    UA_DateTime time;
};

/** Repeated events must ensure that the data is properly deleted at close */
struct repeatedEvent {
    struct timedEvent next;
    UA_Guid id;
    UA_UInt32 interval; // in 100ns resolution
};

/**
 * Collect data that shall be freed after all current workitems have finished.
 *
 * Correct waiting is ensured by letting a token run through the dispatch queue.
 * When the token is processed, all current workitems have been dispatched. Then
 * we wait for the worker threads to increase their local counter.
 */
struct delayedFreeGather {
    UA_Byte tokenStatus; // 0: Nothing, 1: Token sent, 2: Token arrived
    UA_DateTime nextTry; // Either the time until the token shall be send, or
                         // the time when the worker thread counter shall be
                         // checked the next time
    UA_UInt32 *workerCounter; // the size is known to the UA_Server_run thread
    UA_UInt16 dataPtrSize;
    void **dataPtr;
};

UA_StatusCode UA_Server_run(UA_Server *server, UA_UInt32 nThreads, UA_Boolean *running) {
#ifdef MULTITHREADING
    server->running = running;
    rcu_register_thread();
    pthread_t *thr = UA_alloca(nThreads * sizeof(pthread_t));
    for(UA_UInt32 i=0;i<nThreads;i++)
        pthread_create(&thr[i], UA_NULL, (void* (*)(void*))runWorkerLoop, server);
#endif
    UA_StatusCode retval = UA_STATUSCODE_GOOD;

    while(*running) {
        // Check if messages have arrived and handle them.
        for(UA_Int32 i=0;i<server->nlsSize;i++) {
            UA_NetworkLayer *nl = &server->nls[i];
            UA_WorkItem *work;
            UA_Int32 workSize = nl->getWork(nl->nlhandle, &work);
            if(workSize<=0)
                continue;
            #ifdef MULTITHREADING
            struct workListNode *wln = UA_alloc(sizeof(struct workListNode));
            if(!wln) {
                // todo: error handling
            }
            *wln = {.workSize = workSize, .work = work};
            cds_wfq_node_init(&wln->node);
            cds_wfq_enqueue(&server->dispatchQueue, &wln->node);
            #else
            processWork(server, work, workSize);
            UA_free(work);
            #endif
        }

        // Check if timeouts for events have been triggered and handle them.
        // Also find the delay until the next event is due.

        // exit if something went horribly wrong
        if(retval)
            break;

        // sleep if nothing happened in this iteration
    }
#ifdef MULTITHREADING
    for(UA_UInt32 i=0;i<nThreads;i++)
        pthread_join(thr[i], UA_NULL);
    rcu_unregister_thread();
#endif
    return retval;
}
