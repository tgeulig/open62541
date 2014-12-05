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

#define MAXBACKLOG 100

struct Networklayer_TCP;

/* Forwarded to the server as a (UA_Connection) and used for callbacks back into
   the networklayer */
typedef struct {
	UA_Connection connection;
	UA_Int32 sockfd;
	struct NetworkLayerTCP *layer;
} TCPConnection;

/* Internal mapping of sockets to connections */
typedef struct {
    TCPConnection *connection;
    UA_Int32 sockfd;
} ConnectionEntry;

typedef struct NetworkLayerTCP {
	UA_ConnectionConfig conf;
	fd_set fdset;
	UA_Int32 serversockfd;
	UA_Int32 highestfd;
    UA_Boolean recomputeFDSet;
    UA_Int32 connsSize;
    ConnectionEntry *conns;
} NetworkLayerTCP;

void closeCallback(TCPConnection *handle);
void writeCallback(TCPConnection *handle, UA_ByteStringArray gather_buf);

static UA_StatusCode NetworkLayerTCP_add(NetworkLayerTCP *layer, UA_Int32 newsockfd) {
    TCPConnection *c = malloc(sizeof(TCPConnection));
	c->sockfd = newsockfd;
    c->layer = layer;
    c->connection.state = UA_CONNECTION_OPENING;
    c->connection.localConf = layer->conf;
    c->connection.channel = UA_NULL;
    c->connection.close = (void (*)(void*))closeCallback;
    c->connection.write = (void (*)(void*, UA_ByteStringArray))writeCallback;

    if(layer->connsSize <= 0) {
        layer->connsSize = 0;
        layer->conns = malloc(sizeof(ConnectionEntry));
    } else 
        layer->conns = realloc(layer->conns, sizeof(ConnectionEntry)*(layer->connsSize+1));
        
    layer->conns[layer->connsSize].connection = c;
    layer->conns[layer->connsSize].sockfd = newsockfd;
    layer->connsSize++;
    layer->recomputeFDSet = 1;
	return UA_STATUSCODE_GOOD;
}

/* copy the connectionentries to a new array, but omit one */
static UA_StatusCode NetworkLayerTCP_remove(NetworkLayerTCP *layer, UA_Int32 sockfd) {
	UA_Int32 i;
	for(i = 0;i < layer->connsSize;i++) {
		if(layer->conns[i].sockfd == sockfd)
			break;
	}
    if(i >= layer->connsSize)
        return UA_STATUSCODE_BADINTERNALERROR;

    layer->connsSize--;
	ConnectionEntry *newc = malloc(sizeof(ConnectionEntry) * layer->connsSize);
	memcpy(newc, layer->conns, sizeof(ConnectionEntry) * i);
	memcpy(&newc[i], &layer->conns[i+1], sizeof(ConnectionEntry) * (layer->connsSize - i));
    free(layer->conns);
	layer->conns = newc;
    layer->recomputeFDSet = 1;
	return UA_STATUSCODE_GOOD;
}

void closeCallback(TCPConnection *handle) {
	shutdown(handle->sockfd,2);
	CLOSESOCKET(handle->sockfd);
    // the connectionentry is removed by the main thread when select indicates a closed function
    NetworkLayerTCP_remove(handle->layer, handle->sockfd);
}

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
	while(nWritten < total_len) {
		UA_UInt32 n=0;
		do {
			result = WSASend(handle->sockfd, buf, gather_buf.stringsSize , (LPDWORD)&n, 0, NULL, NULL);
			if(result != 0)
				printf("NL_TCP_Writer - Error WSASend, code: %d \n", WSAGetLastError());
		} while(errno == EINTR);
		nWritten += n;
	}
#else
	struct iovec iov[gather_buf.stringsSize];
	for(UA_UInt32 i=0;i<gather_buf.stringsSize;i++) {
		iov[i] = (struct iovec) {.iov_base = gather_buf.strings[i].data,
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

		if(n >= 0) {
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
    // do not free(gather_buf.strings). they are assigned on the stack with alloca
}

static UA_StatusCode setNonBlocking(int sockid) {
#ifdef WIN32
	u_long iMode = 1;
	if(IOCTLSOCKET(sockid, FIONBIO, &iMode) != NO_ERROR)
		return UA_STATUSCODE_BADINTERNALERROR;
#else
	int opts = fcntl(sockid,F_GETFL);
	if(opts < 0 || fcntl(sockid,F_SETFL,opts|O_NONBLOCK) < 0)
		return UA_STATUSCODE_BADINTERNALERROR;
#endif
	return UA_STATUSCODE_GOOD;
}

// after every select, reset the set of sockets we want to listen on
static void setFDSet(NetworkLayerTCP *layer) {
	FD_ZERO(&layer->fdset);
	FD_SET(layer->serversockfd, &layer->fdset);
	layer->highestfd = layer->serversockfd;
	for(UA_Int32 i=0;i<layer->connsSize;i++) {
		FD_SET(layer->conns[i].sockfd, &layer->fdset);
		if(layer->conns[i].sockfd > layer->highestfd)
			layer->highestfd = layer->conns[i].sockfd;
	}
    layer->recomputeFDSet = 0;
}

UA_Int32 NetworkLayerTCP_getWork(NetworkLayerTCP * layer, UA_WorkItem **workItems);
UA_Int32 NetworkLayerTCP_shutdown(NetworkLayerTCP * layer, UA_WorkItem **workItems);
void NetworkLayerTCP_delete(NetworkLayerTCP *layer);

UA_NetworkLayer * NetworkLayerTCP_new(UA_ConnectionConfig conf, UA_UInt32 port) {
    NetworkLayerTCP *layer = malloc(sizeof(NetworkLayerTCP));
    if(!layer)
        return NULL;
	layer->conf = conf;
	layer->connsSize = 0;
	layer->conns = NULL;
    layer->recomputeFDSet = 1;

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
	serv_addr.sin_port = htons(port);

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

	setNonBlocking(layer->serversockfd);
	listen(layer->serversockfd, MAXBACKLOG);

    UA_NetworkLayer *nl = malloc(sizeof(UA_NetworkLayer));
    if(!nl)
        return NULL;
    nl->nlhandle = layer;
    nl->getWork = (UA_Int32 (*)(void*, UA_WorkItem**)) NetworkLayerTCP_getWork;
    nl->delete = (void (*)(void*))NetworkLayerTCP_delete;
    return nl;
}

// send all open conns to the server to delete them before!!
void NetworkLayerTCP_delete(NetworkLayerTCP *layer) {
	for(UA_Int32 index = 0;index < layer->connsSize;index++) {
		shutdown(layer->conns[index].sockfd, 2);
		CLOSESOCKET(layer->conns[index].sockfd);
	}
	free(layer->conns);
	free(layer);
#ifdef WIN32
	WSACleanup();
#endif
}

UA_Int32 NetworkLayerTCP_getWork(NetworkLayerTCP * layer, UA_WorkItem **workItems) {
    //if(layer->recomputeFDSet)
        setFDSet(layer);
    struct timeval tmptv = {0, 0}; // don't wait
    UA_Int32 resultsize = select(layer->highestfd+1, &layer->fdset, NULL, NULL, &tmptv);
    if (resultsize <= 0)
        return -1; // todo: error handling
        
	// accept new conns (can only be a single one)
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

    if(resultsize <= 0) return -1; // todo: error handling
    UA_WorkItem *items = malloc(sizeof(UA_WorkItem)*resultsize);
    if(!items) return -1;

	// read from established sockets
    UA_Int32 j = 0;
    UA_ByteString buf = {-1, NULL};
	for(UA_Int32 i=0;i<layer->connsSize && j<resultsize;i++) {
		if(FD_ISSET(layer->conns[i].sockfd, &layer->fdset)) {
            if(buf.data == NULL)
                buf.data = malloc(layer->conf.recvBufferSize);
#ifdef WIN32
            buf.length = recv(layer->conns[i].connection.sockfd, (char *)buf.data,
                              layer->conf.recvBufferSize, 0);
#else
            buf.length = read(layer->conns[i].sockfd, buf.data,
                              layer->conf.recvBufferSize);
#endif
            if (errno != 0) {
                items[j].type = UA_WORKITEMTYPE_BINARYNETWORKCLOSED;
                items[j].item.binaryNetworkClose.connection = (UA_Connection*)layer->conns[i].connection;
                j++;
                continue;
            }

            items[j].type = UA_WORKITEMTYPE_BINARYNETWORKMESSAGE;
            items[j].item.binaryNetworkMessage.message = buf;
            items[j].item.binaryNetworkMessage.connection = (UA_Connection*)layer->conns[i].connection;
            j++;
            buf.data = NULL;
        }
    }

    if(buf.data != NULL)
        free(buf.data);

    *workItems = items;
    return resultsize;
}
