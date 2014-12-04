/*
 * This work is licensed under a Creative Commons CCZero 1.0 Universal License.
 * See http://creativecommons.org/publicdomain/zero/1.0/ for more information.
 */

#ifdef WIN32
#include <malloc.h>
#include <winsock2.h>
#include <sys/types.h>
#include <Windows.h>
#include <ws2tcpip.h>
#define CLOSESOCKET(S) closesocket(S)
#define IOCTLSOCKET ioctlsocket
#else
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/select.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/socketvar.h>
#include <unistd.h> // read, write, close
#define CLOSESOCKET(S) close(S)
#define IOCTLSOCKET ioctl
#endif

#include <stdlib.h> // exit
#include <stdio.h>
#include <errno.h> // errno, EINTR
#include <memory.h> // memset
#include <fcntl.h> // fcntl

#include "networklayer_tcp.h"

struct Networklayer_TCP;
typedef struct TCPConnection {
	UA_Connection connection;
	UA_Int32 sockfd;
	struct NetworkLayerTCP *layer;
} TCPConnection;

struct ConnectionEntry {
    TCPConnection *connection;
    UA_Int32 sockfd;
};

typedef struct NetworkLayerTCP {
	UA_ConnectionConfig localConf;
	UA_UInt32 port;
	fd_set fdset;
	UA_Int32 serversockfd;
	UA_Int32 highestfd;
    UA_Boolean recomputeFDSet;
    UA_Int32 connectionsSize;
    struct ConnectionEntry *connections;
} NetworkLayerTCP;

void closeCallback(TCPConnection *handle);
void writeCallback(TCPConnection *handle, UA_ByteStringArray gather_buf);

static UA_StatusCode NetworkLayerTCP_add(NetworkLayerTCP *layer, UA_Int32 newsockfd) {
    TCPConnection *c;
    if(!(c = malloc(sizeof(TCPConnection))))
        return UA_STATUSCODE_BADOUTOFMEMORY;

	c->sockfd = newsockfd;
    c->layer = layer;
    c->connection.state = UA_CONNECTION_OPENING;
    c->connection.localConf = layer->localConf;
    c->connection.channel = UA_NULL;
    c->connection.close = close;
    c->connection.write = write;

    layer->connections = realloc(layer->connections, sizeof(struct ConnectionEntry) * (layer->connectionsSize+1));
    if(!layer->connections) {
        free(c);
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }
        
    layer->connections[layer->connectionsSize].connection = c;
    layer->connections[layer->connectionsSize].sockfd = newsockfd;
    layer->connectionsSize++;
	return UA_STATUSCODE_GOOD;
}

// copy the array of connections, but _loose_ one. This does not close the
// actual socket.
static UA_StatusCode NetworkLayerTCP_remove(NetworkLayerTCP *layer, UA_Int32 sockfd) {
	UA_UInt32 index;
	for(index = 0;index < layer->connectionsSize;index++) {
		if(layer->connections[index].sockfd == sockfd)
			break;
	}

    if(index == layer->connectionsSize)
        return UA_STATUSCODE_BADINTERNALERROR;

    layer->connectionsSize--;
	struct ConnectionEntry *newconnections = malloc(sizeof(struct ConnectionEntry) * layer->connectionsSize);

    if(!newconnections)
        return UA_STATUSCODE_BADOUTOFMEMORY;

	memcpy(newconnections, layer->connections, sizeof(struct ConnectionEntry) * index);
	memcpy(&newconnections[index], &layer->connections[index+1],
           sizeof(struct ConnectionEntry) * (layer->connectionsSize - index));
    free(layer->connections);
	layer->connections = newconnections;
	return UA_STATUSCODE_GOOD;
}

/** Callback function */
void closeCallback(TCPConnection *handle) {
	shutdown(handle->sockfd,2);
	CLOSESOCKET(handle->sockfd);
    // the connectionentry is removed by the main thread when select indicates a closed function
    // NetworkLayerTCP_remove(handle->layer, handle->sockfd);
}

/** Callback function */
void writeCallback(TCPConnection *handle, UA_ByteStringArray gather_buf) {
	UA_UInt32 total_len = 0;
	UA_UInt32 nWritten = 0;
#ifdef WIN32
	LPWSABUF buf = _alloca(gather_buf.stringsSize * sizeof(WSABUF));
	int result = 0;
	for(UA_UInt32 i = 0; i<gather_buf.stringsSize; i++) {
		buf[i].buf = gather_buf.strings[i].data;
		buf[i].len = gather_buf.strings[i].length;
		total_len += gather_buf.strings[i].length;
	}
	while (nWritten < total_len) {
		UA_UInt32 n=0;
		do {
			result = WSASend(handle->sockfd, buf, gather_buf.stringsSize , (LPDWORD)&n, 0, NULL, NULL);
			if(result != 0)
				printf("NL_TCP_Writer - Error WSASend, code: %d \n", WSAGetLastError());
		} while (errno == EINTR);
		nWritten += n;
	}
#else
	struct iovec iov[gather_buf.stringsSize];
	for(UA_UInt32 i=0;i<gather_buf.stringsSize;i++) {
		iov[i] = {.iov_base = gather_buf.strings[i].data,
                  .iov_len = gather_buf.strings[i].length};
		total_len += gather_buf.strings[i].length;
	}
	struct msghdr message = {.msg_name = NULL, .msg_namelen = 0, .msg_iov = iov,
							 .msg_iovlen = gather_buf.stringsSize, .msg_control = NULL,
							 .msg_controllen = 0, .msg_flags = 0};
	while (nWritten < total_len) {
		UA_Int32 n = 0;
		do {
            n = sendmsg(handle->sockfd, &message, 0);
        } while (n == -1L && errno == EINTR);

		if (n >= 0) {
			// TODO: handle incompletely send messages
			/* nWritten += n; */
			break;
		} else {
			// TODO: error handling
			break;
		}
	}
#endif
    for(UA_UInt32 i=0;i<gather_buf.stringsSize;i++)
        free(gather_buf.strings[i].data);
}

static UA_StatusCode setNonBlocking(int sockid) {
#ifdef WIN32
	u_long iMode = 1;
	if (IOCTLSOCKET(sockid, FIONBIO, &iMode) != NO_ERROR)
		return UA_STATUSCODE_BADINTERNALERROR;
#else
	int opts = fcntl(sockid,F_GETFL);
	if (opts < 0 || fcntl(sockid,F_SETFL,opts|O_NONBLOCK) < 0)
		return UA_STATUSCODE_BADINTERNALERROR;
#endif
	return UA_STATUSCODE_GOOD;
}

// after every select, reset the set of sockets we want to listen on
static void setFDSet(NetworkLayerTCP *layer) {
	FD_ZERO(&layer->fdset);
	FD_SET(layer->serversockfd, &layer->fdset);
	layer->highestfd = layer->serversockfd;
	for(UA_UInt32 i=0;i<layer->connectionsSize;i++) {
		FD_SET(layer->connections[i].sockfd, &layer->fdset);
		if(layer->connections[i].sockfd > layer->highestfd)
			layer->highestfd = layer->connections[i].sockfd;
	}
}

void NetworkLayerTCP_delete(NetworkLayerTCP *layer);
UA_WorkItemList NetworkLayerTCP_getWorkItems(NetworkLayerTCP * layer);

UA_NetworkLayer * NetworkLayerTCP_new(UA_ConnectionConfig localConf, UA_UInt32 port) {
    NetworkLayerTCP *layer = malloc(sizeof(NetworkLayerTCP));
    if(layer == NULL)
        return NULL;
	layer->localConf = localConf;
	layer->port = port;
	layer->connectionsSize = 0;
	layer->connections = NULL;

#ifdef WIN32
	WORD wVersionRequested;
	WSADATA wsaData;
	wVersionRequested = MAKEWORD(2, 2);
	WSAStartup(wVersionRequested, &wsaData);
	if((layer->serversockfd = socket(PF_INET, SOCK_STREAM,0)) == INVALID_SOCKET) {
		printf("ERROR opening socket, code: %d\n", WSAGetLastError());
		return NULL;
	}
#else
    if((layer->serversockfd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
		perror("ERROR opening socket");
		return NULL;
	} 
#endif

	struct sockaddr_in serv_addr;
	memset((void *)&serv_addr, sizeof(serv_addr), 1);
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(layer->port);

	int optval = 1;
	if(setsockopt(layer->serversockfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&optval,
                  sizeof(optval)) == -1) {
		perror("setsockopt");
		CLOSESOCKET(layer->serversockfd);
		return NULL;
	}
		
	if(bind(layer->serversockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
		perror("binding");
		CLOSESOCKET(layer->serversockfd);
		return NULL;
	}

#define MAXBACKLOG 100
	setNonBlocking(layer->serversockfd);
	listen(layer->serversockfd, MAXBACKLOG);

    UA_NetworkLayer *nl = malloc(sizeof(UA_NetworkLayer));
    if(!nl)
        return NULL;
    nl->networkLayerHandle = layer;
    nl->getWorkItems = (UA_WorkItemList (*)(void*)) NetworkLayerTCP_getWorkItems;
    nl->delete = (void (*)(void*))NetworkLayerTCP_delete;
    return nl;
}

// send all open connections to the server to delete them before!!
void NetworkLayerTCP_delete(NetworkLayerTCP *layer) {
	for(UA_UInt32 index = 0;index < layer->connectionsSize;index++) {
		shutdown(layer->connections[index].sockfd, 2);
		CLOSESOCKET(layer->connections[index].sockfd);
	}
	free(layer->connections);
	free(layer);
#ifdef WIN32
	WSACleanup();
#endif
}

UA_WorkItemList NetworkLayerTCP_getWorkItems(NetworkLayerTCP * layer) {
    setFDSet(layer);
    struct timeval tmptv = {0, 0}; // don't wait
    UA_Int32 resultsize = select(layer->highestfd+1, &layer->fdset, NULL, NULL, &tmptv);
    if (resultsize <= 0)
        return (UA_WorkItemList){.workItemsSize = 0, .workItems = NULL}; // todo: error handling
        
	// accept new connections (can only be a single one)
	if(FD_ISSET(layer->serversockfd,&layer->fdset)) {
		struct sockaddr_in cli_addr;
		socklen_t cli_len = sizeof(cli_addr);
		int newsockfd = accept(layer->serversockfd, (struct sockaddr *) &cli_addr, &cli_len);
		if (newsockfd >= 0) {
			setNonBlocking(newsockfd);
			NetworkLayerTCP_add(layer, newsockfd);
		}
		resultsize--;
	}

    if(resultsize <= 0)
        return (UA_WorkItemList){.workItemsSize = 0, .workItems = NULL}; // todo: error handling

    UA_WorkItem *items = malloc(sizeof(UA_WorkItem)*resultsize);

	// read from established sockets
    UA_Int32 j=0;
    UA_ByteString buf = {-1, NULL};
	for(UA_UInt32 i=0;i<layer->connectionsSize && j<resultsize;i++) {
		if(FD_ISSET(layer->connections[i].sockfd, &layer->fdset)) {
            if(buf.data == NULL)
                buf.data = malloc(layer->localConf.recvBufferSize);
#ifdef WIN32
            buf.length = recv(layer->connections[i].connection.sockfd, (char *)buf.data,
                              layer->localConf.recvBufferSize, 0);
#else
            buf.length = read(layer->connections[i].sockfd, buf.data,
                              layer->localConf.recvBufferSize);
#endif
            if (errno != 0) {
                items[j].workItemType = UA_WORKITEMTYPE_NETWORKCLOSED;
                // FIXME: the location of the connection could change
                items[j].item.networkClose.connection = layer->connections[i].connection;
                j++;
                continue;
            }

            items[j].workItemType = UA_WORKITEMTYPE_NETWORKMESSAGE;
            items[j].item.networkMessage.message = buf;
            // FIXME: the location of the connection could change
            items[j].item.networkMessage.connection = layer->connections[i].connection;
            j++;
            buf.data = NULL;
        }
    }

    if(buf.data != NULL)
        free(buf.data);

    return (UA_WorkItemList){.workItemsSize = resultsize, .workItems = items};
}
