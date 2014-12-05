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
                item->item.binaryNetworkClose.connection->channel->connection = UA_NULL;
            UA_free(item->item.binaryNetworkClose.connection);
            break;
        case UA_WORKITEMTYPE_METHODCALL:
            item->item.methodCall.method(server, item->item.methodCall.data);
            break;
        default:
            assert(UA_FALSE);
        }
    }
}

/* Sometimes we need to know whether all tasks that were added to the dispatch
   queue before have been finished. For example when a UC_Connection closes and
   we need to ensure that all workitems that use the connection have finished so
   we can free it.

   The easiest approach would be to add a special workitem with a callback to
   delete the connection at the end of the queue. But we can't know that the
   concurrent events have finished on parallel threads (processors) when the
   workitem is dispateched.

   To ensure that the tasks already put in the queue have finished, every thread
   increases a counter whenever it takes a new worklist or has a timeout on the
   mutex.

   Tasks that shall be executed

   every task has increased its counter by at least two. That
   means,

   

 */

#ifdef MULTITHREADING
void * runWorkerLoop(UA_Server *server) {
   	rcu_register_thread();
    while(*(server->running)) {
        struct cds_wfq_node *node = cds_wfq_dequeue_blocking(&server->dispatchQueue);
        if(!node)
            continue;
        struct _workListNode *wln = (struct _workListNode*)node;
        processWorkList(server,&wln->workList);
        UA_free((void *)wln->workList.workItems);
        UA_free(wln);
    }
   	rcu_unregister_thread();
    return UA_NULL;
}
#endif
