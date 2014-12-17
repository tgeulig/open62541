#include "ua_server_internal.h"

static void processWorkItems(UA_Server *server, const UA_WorkItem *work, UA_Int32 workSize) {
    for(UA_Int32 i = 0;i<workSize;i++) {
        const UA_WorkItem *item = &work[i];
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
                item->item.binaryNetworkClosed->channel->connection = UA_NULL;
            break;
        case UA_WORKITEMTYPE_METHODCALL:
            item->item.methodCall.method(server, item->item.methodCall.data);
            break;
        case UA_WORKITEMTYPE_DELAYEDFREE:
            UA_free(item->item.delayedFree);
        default: {}
            // do nothing
        }
    }
}

#ifdef UA_MULTITHREADING

/**
 * Worker Loop
 *
 * The workers wait in the dispatch queue until work arrives. When done, they
 * increase their local workerCounter and wait for the next work.
 */

/** Entry in the dipatch queue */
struct workListNode {
    struct cds_wfcq_node node; // node for the queue
    UA_UInt32 workSize;
    UA_WorkItem *work;
};

// throwaway struct to bring data into the worker threads
struct workerStartData {
    UA_Server *server;
    UA_UInt32 **workerCounter;
};

/** Waits until work arrives in the dispatch queue (restart after 10ms) and processes it. */
static void * workerLoop(struct workerStartData *startInfo) {
    UA_Server_registerThread();
    UA_UInt32 *c = UA_alloc(sizeof(UA_UInt32));
    uatomic_set(c, 0);

    *startInfo->workerCounter = c;
    UA_Server *server = startInfo->server;
    UA_free(startInfo);
    
    while(*server->running) {
        struct workListNode *wln = (struct workListNode*)
            cds_wfcq_dequeue_blocking(&server->dispatchQueue_head, &server->dispatchQueue_tail);
        if(wln) {
            processWorkItems(server, wln->work, wln->workSize);
            UA_free(wln->work);
            UA_free(wln);
        }
        uatomic_inc(c); // increase the workerCounter;
    }
    UA_Server_unregisterThread();
    return UA_NULL;
}

void UA_Server_emptyDispatchQueue(UA_Server *server) {

}

#define BATCHSIZE 20

/** Dispatch work to workers. Slices the work up if it contains more than
    BATCHSIZE items. The work array is freed by the worker threads. */
static void dispatchWork(UA_Server *server, UA_Int32 workSize, UA_WorkItem *work) {
    UA_Int32 startIndex = workSize; // start at the end
    while(workSize > 0) {
        UA_Int32 size = BATCHSIZE;
        if(size > workSize)
            size = workSize;
        startIndex = startIndex - size;
        struct workListNode *wln = UA_alloc(sizeof(struct workListNode));
        if(startIndex > 0) {
            UA_WorkItem *workSlice = UA_alloc(size * sizeof(UA_WorkItem));
            UA_memcpy(workSlice, &work[startIndex], size * sizeof(UA_WorkItem));
            *wln = (struct workListNode){.workSize = size, .work = workSlice};
        }
        else
            *wln = (struct workListNode){.workSize = size, .work = work}; // do not alloc, but forward the original array
        cds_wfcq_node_init(&wln->node);
        cds_wfcq_enqueue(&server->dispatchQueue_head, &server->dispatchQueue_tail, &wln->node);
        workSize -= size;
    } 
}

#endif

/**
 * Delayed Work
 *
 * Sometimes, a method call needs to wait until all work items that were
 * dispatched earlier have finished. For example, to remove a UA_Connection, no
 * prior workitem may still posess a pointer to the connection. We collect
 * "delayed work" and regularly check if it can be executed.
 *
 * Do not use for time-critical interactions. Only for internal clean-up.
 */

#define DELAYEDWORKSIZE 100 // Collect delayed work until we have DELAYEDWORKSIZE items

struct UA_DelayedWork {
    TAILQ_ENTRY(UA_DelayedWork) pointers;
    UA_UInt32 *workerCounters; // initially UA_NULL until a workitem gets the counters
    UA_UInt32 workItemsCount; // the size of the array is DELAYEDWORKSIZE, the count may be less
    UA_WorkItem *workItems; // when it runs full, a new delayedWork entry is created
};

#ifdef UA_MULTITHREADING

// Dispatched as a methodcall-WorkItem when the delayedwork is added
static void getCounter(UA_Server *server, UA_DelayedWork *delayed) {
    UA_UInt32 *counters = UA_alloc(server->nThreads * sizeof(UA_UInt32));
    for(UA_UInt16 i = 0;i<server->nThreads;i++)
        delayed->workerCounters[i] = *server->workerCounters[i];
    delayed->workerCounters = counters;
}

// Execute this every N seconds (repeated work) to execute delayed work that is ready
void UA_Server_processDelayedWorkQueue(UA_Server *server) {
    UA_DelayedWork *dw;
    while((dw = TAILQ_LAST(&server->delayedWork, UA_DelayedWorkQueue))) {
        // If the counters have not been set, the delayed work earlier in the
        // list is considered not ready either
        if(!dw->workerCounters)
            break;

        // Test if every workerCounter was changed at least once -> worker finished
        // the work he had when the getCounter method was dispatched
        for(UA_UInt16 i=0;i<server->nThreads;i++) {
            if(*server->workerCounters[i] == dw->workerCounters[i])
                break;
        }

        dispatchWork(server, dw->workItemsCount, dw->workItems);
        TAILQ_REMOVE(&server->delayedWork, dw, pointers);
        UA_free(dw->workerCounters);
        UA_free(dw);
    }
}

#endif

/**
 * Timed Work
 *
 * Timed work is stored in a linked list of workitem arrays. The linked list is
 * ordered by execution time. If a repetition interval is given, the entry is
 * copied with a new execution time before dispatch.
 *
 * When an entry with a repetitionInterval is added, we try to attach it to
 * entries with the same interval, even if the next execution is sooner than
 * normal.
 */
struct UA_TimedWork {
    LIST_ENTRY(UA_TimedWork) pointers;
    UA_UInt16 workSize;
    UA_WorkItem *work;
    UA_Guid *workIds;
    UA_DateTime time;
    UA_UInt32 repetitionInterval; // in 100ns resolution, 0 means no repetition
};

/* The item is copied and not freed by this function. */

static UA_Guid addTimedWork(UA_Server *server, UA_WorkItem *item, UA_DateTime firstTime, UA_UInt32 repetitionInterval) {
    UA_TimedWork *tw, *lastTw = UA_NULL;

    // search for matching entry
    LIST_FOREACH(tw, &server->timedWork, pointers) {
        if(tw->repetitionInterval == repetitionInterval && (repetitionInterval > 0 || tw->time == firstTime))
            break; // found a matching entry
        if(tw->time > firstTime) {
            tw = UA_NULL; // not matchin entry exists
            lastTw = tw;
            break;
        }
    }
    
    if(tw) {
        // append to matching entry
        tw->workSize++;
        tw->work = UA_realloc(tw->work, sizeof(UA_WorkItem)*tw->workSize);
        tw->workIds = UA_realloc(tw->workIds, sizeof(UA_Guid)*tw->workSize);
        tw->work[tw->workSize-1] = *item;
        tw->workIds[tw->workSize-1] = UA_Guid_random(&server->random_seed);
        return tw->workIds[tw->workSize-1];
    }

    // create a new entry
    tw = UA_alloc(sizeof(UA_TimedWork));
    tw->workSize = 1;
    tw->time = firstTime;
    tw->repetitionInterval = repetitionInterval;
    tw->work = UA_alloc(sizeof(UA_WorkItem));
    tw->work[0] = *item;
    tw->workIds = UA_alloc(sizeof(UA_Guid));
    tw->workIds[0] = UA_Guid_random(&server->random_seed);
    if(lastTw)
        LIST_INSERT_AFTER(lastTw, tw, pointers);
    else
        LIST_INSERT_HEAD(&server->timedWork, tw, pointers);

    return tw->workIds[0];
}

// Currently, these functions need to get the server mutex, but should be sufficiently fast
UA_Guid UA_Server_addTimedWorkItem(UA_Server *server, UA_WorkItem *work, UA_DateTime time) {
    return addTimedWork(server, work, time, 0);
}

UA_Guid UA_EXPORT UA_Server_addRepeatedWorkItem(UA_Server *server, UA_WorkItem *work, UA_UInt32 interval) {
    return addTimedWork(server, work, UA_DateTime_now() + interval, interval);
}

UA_Boolean UA_EXPORT UA_Server_removeWorkItem(UA_Server *server, UA_Guid workId);

/** Dispatches timed work, returns the timeout until the next timed work in ms */
static UA_UInt16 processTimedWork(UA_Server *server) {
    UA_DateTime current = UA_DateTime_now();
    UA_TimedWork *next = LIST_FIRST(&server->timedWork);
    UA_TimedWork *tw = UA_NULL;

    while(next) {
        tw = next;
        if(tw->time > current)
            break;
        next = LIST_NEXT(tw, pointers);

#ifdef UA_MULTITHREADING
        if(tw->repetitionInterval > 0) {
            // copy the entry and insert at the new location
            UA_WorkItem *workCopy = UA_alloc(sizeof(UA_WorkItem) * tw->workSize);
            UA_memcpy(workCopy, tw->work, sizeof(UA_WorkItem) * tw->workSize);
            dispatchWork(server, tw->workSize, workCopy); // frees the work pointer

            UA_TimedWork *prevTw = tw; // after which tw do we insert?
            while(UA_TRUE) {
                UA_TimedWork *next = LIST_NEXT(prevTw, pointers);
                if(!next || next->time > tw->time)
                    break;
                prevTw = next;
            }
            if(prevTw != tw) {
                LIST_REMOVE(tw, pointers);
                LIST_INSERT_AFTER(prevTw, tw, pointers);
            }
        } else {
            dispatchWork(server, tw->workSize, tw->work); // frees the work pointer
            LIST_REMOVE(tw, pointers);
            UA_free(tw->workIds);
            UA_free(tw);
        }
#else
        processWorkItems(server,tw->work, tw->workSize); // does not free the work
        if(tw->repetitionInterval > 0) {
            tw->time += tw->repetitionInterval;
            UA_TimedWork *prevTw = tw;
            while(UA_TRUE) {
                UA_TimedWork *next = LIST_NEXT(prevTw, pointers);
                if(!next || next->time > tw->time)
                    break;
                prevTw = next;
            }
            if(prevTw != tw) {
                LIST_REMOVE(tw, pointers);
                LIST_INSERT_AFTER(prevTw, tw, pointers);
            }
        } else {
            LIST_REMOVE(tw, pointers);
            UA_free(tw->work);
            UA_free(tw->workIds);
            UA_free(tw);
        }
#endif
    }

    tw = LIST_FIRST(&server->timedWork);
    UA_UInt16 timeout = UA_UINT16_MAX;
    if(tw)
        timeout = (tw->time - UA_DateTime_now())/10;
    return timeout;
}

void UA_Server_deleteTimedWork(UA_Server *server) {
    UA_TimedWork *tw;
    while((tw = LIST_FIRST(&server->timedWork))) {
        LIST_REMOVE(tw, pointers);
        UA_free(tw->work);
        UA_free(tw->workIds);
        UA_free(tw);
    }
}

/**
 * # Main server thread
 *
 * ## Handling of timed work
 *
 * ## Handling of delayed work
 * The server always has a delayedWorkObject that is not in the queue. There,
 * delayed work is added until it runs full. Then it gets added to the queue and
 * a methodcall to getCounter is dispatched. Also, a repeatedEvent calls
 * walkDelayedWorkQueue every 100ms.
 */

#define MAXTIMEOUT 5 // max timeout until the next main loop iteration

UA_StatusCode UA_Server_run(UA_Server *server, UA_UInt16 nThreads, UA_Boolean *running) {
#ifdef UA_MULTITHREADING
    // 1) Prepare the threads
    server->running = running; // the threads need to access the variable
    server->nThreads = nThreads;
    pthread_t *thr = UA_alloca(nThreads * sizeof(pthread_t));
    server->workerCounters = UA_alloc(nThreads * sizeof(UA_UInt32 *));
    for(UA_UInt32 i=0;i<nThreads;i++) {
        struct workerStartData *startData = UA_alloc(sizeof(struct workerStartData));
        startData->server = server;
        startData->workerCounter = &server->workerCounters[i];
        pthread_create(&thr[i], UA_NULL, (void* (*)(void*))workerLoop, startData);
    }
#endif

    // 2) Start the networklayers
    for(UA_Int32 i=0;i<server->nlsSize;i++) {
        server->nls[i].start(server->nls[i].nlHandle);
    }

    // 2) Queue-List of delayedWork
    /* UA_DelayedWork *dw = UA_alloc(sizeof(UA_DelayedWork)); */
    /* dw->workItems = UA_alloc(sizeof(UA_WorkItem) * DELAYEDWORKSIZE); */
    /* dw->workerCounters = UA_NULL; */
    /* dw->workItemsCount = 0; */
    /* TAILQ_INSERT_HEAD(&server->delayedWork, dw, pointers); */

    UA_UInt16 timeout = 0; // timeout for the last networklayer in ms
    while(*running) {
        // Check if messages have arrived and handle them.
        for(UA_Int32 i=0;i<server->nlsSize;i++) {
            UA_NetworkLayer *nl = &server->nls[i];
            UA_WorkItem *work;
            UA_Int32 workSize;
            if(i == server->nlsSize-1)
                workSize = nl->getWork(nl->nlHandle, &work, 0);
            else
                workSize = nl->getWork(nl->nlHandle, &work, timeout);

#ifdef UA_MULTITHREADING
            dispatchWork(server, workSize, work);
#else
            if(workSize > 0) {
                processWorkItems(server, work, workSize);
                UA_free(work);
            }
#endif
        }
        timeout = processTimedWork(server);
        if(timeout > MAXTIMEOUT)
            timeout = MAXTIMEOUT;
    }

#ifdef UA_MULTITHREADING
    for(UA_UInt32 i=0;i<nThreads;i++) {
        pthread_join(thr[i], UA_NULL);
        UA_free(server->workerCounters[i]);
    }
    UA_free(server->workerCounters);
#endif

    // todo: process the delayed work in the main thread

    // Stop the networklayers and process the necessary work in this thread
    for(UA_Int32 i=0;i<server->nlsSize;i++) {
        UA_WorkItem *stopNLWork;
        UA_Int32 stopNLWorkSize = server->nls[i].stop(server->nls[i].nlHandle, &stopNLWork);
        processWorkItems(server, stopNLWork, stopNLWorkSize);
        UA_free(stopNLWork);
    }

    // todo: empty the dispatch queue. Delete the work that is stuck in there.

    return UA_STATUSCODE_GOOD;
}
