/**
 * @file
 * Local and remote node macros and functions
 */

/*
 *******************************************************************
 *
 * Copyright 2016 Intel Corporation All rights reserved.
 *
 *-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
 */

#ifndef _DPS_NODE_H
#define _DPS_NODE_H

#include <safe_lib.h>
#include <dps/private/network.h>
#include <uv.h>
#include "bitvec.h"
#include "cose.h"
#include "history.h"
#include "queue.h"

#if UV_VERSION_MAJOR < 1 || UV_VERSION_MINOR < 15
#error libuv version 1.15 or higher is required
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define DPS_MSG_VERSION 1 /**< DPS protocol version */

/*
 * DPS message types
 */
#define DPS_MSG_TYPE_PUB  1   /**< Publication */
#define DPS_MSG_TYPE_SUB  2   /**< Subscription */
#define DPS_MSG_TYPE_ACK  3   /**< End-to-end publication acknowledgement */
#define DPS_MSG_TYPE_SAK  4   /**< One-hop subscription acknowledgement */

#define DPS_NODE_CREATED      0 /**< Node is created */
#define DPS_NODE_RUNNING      1 /**< Node is running */
#define DPS_NODE_STOPPING     2 /**< Node is stopping */
#define DPS_NODE_STOPPED      3 /**< Node is stopped */
#define DPS_NODE_PAUSED       4 /**< Node is paused (not receiving input) */

#if !defined(DOXYGEN_SKIP_FORWARD_DECLARATION)
typedef struct _RemoteNode RemoteNode;
typedef struct _PublicationAck PublicationAck;
#endif

/**
 * Completion context for link and unlink operations
 */
typedef struct _OnOpCompletion OnOpCompletion;

/**
 * Context for network address resolution
 */
typedef struct _ResolverInfo ResolverInfo;

typedef struct _NodeRequest NodeRequest;

/**
 * A request to run on the node thread.
 *
 * @param req the request
 */
typedef void (*OnNodeRequest)(NodeRequest* req);

/**
 * A queue of requests to run on the node thread.
 */
typedef struct _NodeRequest {
    DPS_Queue queue;  /**< The queue item */
    DPS_Node* node;   /**< The node */
    OnNodeRequest cb; /**< The request callback */
    void* data;       /**< The request data */
} NodeRequest;

/**
 * Initialize a request to run on the node thread.
 *
 * @param node the node
 * @param req the request
 * @param cb the request callback, run on the node thread
 */
void DPS_NodeRequestInit(DPS_Node* node, NodeRequest* req, OnNodeRequest cb);

/**
 * Schedule a request to run on the node thread.
 *
 * @param req the request
 *
 * @return DPS_OK if successful, an error otherwise
 */
DPS_Status DPS_NodeRequestSchedule(NodeRequest* req);

/**
 * Cancel a request scheduled to run on the node thread.
 *
 * @param req a previously scheduled request
 */
void DPS_NodeRequestCancel(NodeRequest* req);

/**
 * Specifies when subscriptions are to be sent
 */
typedef enum {
    SubsNonePending,   /**< No subscriptions are pending */
    SubsSendNow,       /**< Pending subscriptions should be sent immediately */
    SubsThrottled      /**< Pending subscriptions will be sent after a delay */
} SubsPendingState;

/**
 * A local node
 */
typedef struct _DPS_Node {
    void* userData;                       /**< Application provided user data */

#ifdef DPS_DEBUG
    uint8_t isLocked;                     /**< Count of node locks */
#endif
    SubsPendingState subsPending;         /**< Specifies when subscriptions are to be sent */
    DPS_NodeAddress addr;                 /**< Listening address */
    char addrStr[DPS_NODE_ADDRESS_MAX_STRING_LEN]; /**< Text of listening address */
    DPS_UUID meshId;                      /**< Randomly allocated mesh id for this node */
    char separators[13];                  /**< List of separator characters */
    DPS_KeyStore *keyStore;               /**< Functions for loading encryption keys */
    COSE_Entity signer;                   /**< Sign messages with this entity */

    uv_thread_t thread;                   /**< Thread for the event loop */
    uv_loop_t* loop;                      /**< uv lib event loop */
    uv_mutex_t nodeMutex;                 /**< Mutex to protect this node */
    uv_mutex_t condMutex;                 /**< Mutex for use with condition variables */

    uv_async_t acksAsync;                 /**< Async for sending acks */
    uv_async_t pubsAsync;                 /**< Async for sending publications */
    uv_timer_t pubsTimer;                 /**< Timer for publication maintenance */
    uv_async_t stopAsync;                 /**< Async for shutting down the node */
    uv_async_t subsAsync;                 /**< Async for sending subscriptions */

    uint32_t subsRate;                    /**< Specifies time delay (in msecs) between subscription updates */
    uint32_t linkLossTimeout;             /**< Specifies the keep alive timeout period */
    uv_timer_t subsTimer;                 /**< Timer for sending subscriptions */

    DPS_Queue ackQueue;                   /**< Queued acknowledgement packets */

    RemoteNode* remoteNodes;              /**< Linked list of remote nodes */

    struct {
        DPS_BitVector* needs;             /**< Preallocated needs bit vector */
        DPS_BitVector* interests;         /**< Preallocated interests bit vector */
    } scratch;                            /**< Preallocated needs and interests */

    DPS_CountVector* interests;           /**< Tracks all interests for this node */
    DPS_CountVector* needs;               /**< Tracks all needs for this node */

    DPS_History history;                  /**< History of recently sent publications */

    DPS_Publication* publications;        /**< Linked list of local and retained publications */
    DPS_Subscription* subscriptions;      /**< Linked list of local subscriptions */

    DPS_MulticastReceiver* mcastReceiver; /**< Multicast receiver context */
    DPS_MulticastSender* mcastSender;     /**< Multicast sender context */
    uint8_t mcastPub;                     /**< Multicast configuration flags */

    DPS_NetContext* netCtx;               /**< Network context */

    uint8_t state;                        /**< Indicates if the node is running, stopping, or stopped */
    DPS_OnNodeShutdown onShutdown;        /**< Function to call when the node is shutdown */
    void* onShutdownData;                 /**< Context to pass to onShutdown callback */
    NodeRequest onShutdownReq;            /**< onShutdown callback request */
    DPS_OnNodeDestroyed onDestroyed;      /**< Function to call when the node is destroyed */
    void* onDestroyedData;                /**< Context to pass to onDestroyed callback */

    uv_async_t resolverAsync;             /**< Async handler for address resolver */
    ResolverInfo* resolverList;           /**< Linked list of address resolution requests */

    uv_signal_t sigusr1;                  /**< Signal handler for dumping node info */

    uv_async_t requestAsync;              /**< Async for running requests on the node thread */
    DPS_Queue requestQueue;               /**< Queue of requests */
    uv_async_t freeAsync;                 /**< Async for freeing subscriptions and publications */
    DPS_Publication* freePubs;            /**< Linked list of freed publications */
    DPS_Subscription* freeSubs;           /**< Linked list of freed subscriptions */

    DPS_OnLinkLoss linkLossCB;            /**< Function called in the case of link-loss */
    void* linkLossData;                   /**< Pointer to be passed when calling linkLossCB() */

    DPS_RBG* rbg;                         /**< Secure random byte generator */

} DPS_Node;

/**
 * Maximum value of a mesh ID
 */
extern const DPS_UUID DPS_MaxMeshId;

/**
 * The remote node state
 */
typedef enum {
    REMOTE_NEW,           /**< Remote node has been created */
    REMOTE_ACTIVE,        /**< Remote node is linked */
    REMOTE_LINKING,       /**< Remote node is in the process of being linked */
    REMOTE_UNLINKING,     /**< Remote node is in the process of being unlinked */
    REMOTE_MUTED,         /**< Remote node was linked but is muted to avoid mesh loops */
    REMOTE_UNMUTING       /**< Remote node is in the process of being unmuted */
} RemoteNodeState;

/**
 * Return the remote node state as a text string
 *
 * @param remote the remote node
 *
 * @return the text string
 */
const char* RemoteStateTxt(const RemoteNode* remote);

/**
 * A remote node
 */
typedef struct _RemoteNode {
    OnOpCompletion* completion;        /**< Completion context for link and unlink operations */
    RemoteNodeState state;             /**< The remote node state */
    /** Inbound state */
    struct {
        uint32_t revision;             /**< Revision number of last subscription received from this node */
        DPS_UUID meshId;               /**< The mesh id received from this remote node */
        DPS_BitVector* needs;          /**< Bit vector of needs received from  this remote node */
        DPS_BitVector* interests;      /**< Bit vector of interests received from  this remote node */
    } inbound;
    /** Outbound state */
    struct {
        uint8_t linkRequested;         /**< TRUE if the local node requested to link to this remote */
        uint8_t deltaInd;              /**< TRUE if the interests info is a delta */
        uint8_t sakCounter;            /**< Counter for deciding when to start and stop resending SUBs and SAKSs */
        uint8_t sendInterests;         /**< TRUE to include interests etc in a SAK */
        uint8_t sakPending;            /**< TRUE when waiting to receive a SAK from this remote node */
        uint8_t lastSubMsgType;        /**< Indicates if last subscription message was a SUB or a SAK */
        uint32_t revision;             /**< Revision number of last subscription sent to this node */
        DPS_BitVector* needs;          /**< Needs bit vector sent outbound to this remote node */
        DPS_BitVector* interests;      /**< Full outbound interests bit vector to this remote node */
        DPS_BitVector* delta;          /**< Delta outbound bit vector sent to this remote node */
    } outbound;
    DPS_NetEndpoint ep;                /**< The endpoint of the remote */
    RemoteNode* next;                  /**< Remotes are a linked list attached to the local node */
} RemoteNode;

/**
 * An opaque pointer of a remote node representing the loopback
 * destination.
 */
extern RemoteNode* DPS_LoopbackNode;

/**
 * Request to asynchronously update subscriptions either
 * immediately or after a timeout expires.
 *
 * @param node       The node
 * @param pending    If FALSE update immediately otherwise wait for the
 *                   subscription update timer to expire.
 */
void DPS_UpdateSubs(DPS_Node* node, SubsPendingState pending);

/**
 * Queue an acknowledgement to be sent asynchronously
 *
 * @param node    The node
 * @param ack     The acknowledgement to queue
 */
void DPS_QueuePublicationAck(DPS_Node* node, PublicationAck* ack);

/**
 * Callback function called when a subscription send operation completes
 *
 * @param node     Opaque pointer to the DPS node
 * @param appCtx   An application context to be passed to the send complete callback
 * @param ep       The endpoint for which the send was completed
 * @param bufs     Array holding pointers to the buffers passed in the send API call. The data in these buffers
 *                 can now be freed.
 * @param numBufs  The length of the bufs array
 * @param status   Indicates if the send was successful or not
 */
void DPS_OnSendSubscriptionComplete(DPS_Node* node, void* appCtx, DPS_NetEndpoint* ep, uv_buf_t* bufs, size_t numBufs, DPS_Status status);

/**
 * Make a nonce for a specific message type
 *
 * @param uuid The publication UUID
 * @param seqNum The publication sequence number
 * @param msgType The message type (DPS_MSG_TYPE_PUB or DPS_MSG_TYPE_ACK)
 * @param alg The recipient algorithm
 * @param rbg a random byte generator
 * @param nonce The computed nonce
 *
 * @return DPS_OK if successful, an error otherwise
 */
DPS_Status DPS_MakeNonce(const DPS_UUID* uuid, uint32_t seqNum, uint8_t msgType,
                         int8_t alg, DPS_RBG* rbg, uint8_t nonce[COSE_NONCE_LEN]);

/**
 * Function to call when a send operation completes.
 *
 * Must be called with the node lock held.
 *
 * @param node     The local node
 * @param addr     The endpoint for which the send failed
 * @param bufs     Array holding pointers to the buffers passed in the send API call. The data in these buffers
 *                 can now be freed.
 * @param numBufs  The length of the bufs array
 * @param status   Indicates the send status
 */
void DPS_SendComplete(DPS_Node* node, DPS_NodeAddress* addr, uv_buf_t* bufs, size_t numBufs, DPS_Status status);

/**
 * Callback function called when a network send operation completes.
 *
 * Acquires the node lock and calls DPS_SendComplete().
 *
 * @param node     Opaque pointer to the DPS node
 * @param appCtx   An application context to be passed to the send complete callback
 * @param ep       The endpoint for which the send was completed
 * @param bufs     Array holding pointers to the buffers passed in the send API call. The data in these buffers
 *                 can now be freed.
 * @param numBufs  The length of the bufs array
 * @param status   Indicates if the send was successful or not
 */
void DPS_OnSendComplete(DPS_Node* node, void* appCtx, DPS_NetEndpoint* ep, uv_buf_t* bufs, size_t numBufs, DPS_Status status);

/**
 * Add an entry for new remote node or return a pointer to the existing remote node.
 *
 * Must be called with the node lock held.
 *
 * @param node      The local node
 * @param addr      The address of the remote node
 * @param cn        Connection state information for the node
 * @param remoteOut Returns an existing or new remote node structure
 *
 * @return
 *          - DPS_OK if a new remote node was added
 *          - DPS_ERR_EXISTS if the node already exists
 *          - Other status codes indicating an error
 */
DPS_Status DPS_AddRemoteNode(DPS_Node* node, const DPS_NodeAddress* addr, DPS_NetConnection* cn, RemoteNode** remoteOut);

/**
 * Lookup a remote node by address.
 *
 * Must be called with the node lock held.
 *
 * @param node    The local node
 * @param addr    The address of the remote node to lookup
 *
 * @return  A pointer to the remote node or NULL if the lookup failed.
 */
RemoteNode* DPS_LookupRemoteNode(DPS_Node* node, const DPS_NodeAddress* addr);

/**
 * Computes the minimum mesh id of the local and all active remote nodes
 * excluding an optional remote node.
 *
 * @param node       The local node
 * @param excluded   A remote node to exclude from the computation
 *
 * @return  The minimum mesh id.
 */
const DPS_UUID* DPS_MinMeshId(const DPS_Node* node, const RemoteNode* excluded);

/**
 * Deletes a remote node and related state information.
 *
 * @param node    The local node
 * @param remote  The remote node to delete
 */
void DPS_DeleteRemoteNode(DPS_Node* node, RemoteNode* remote);

/**
 * Complete an asynchronous operation on a remote node
 *
 * @param completion  The operation completion
 * @param status      Status code indicating the success or failure of the operation
 */
void DPS_RemoteCompletion(OnOpCompletion* completion, DPS_Status status);

/**
 * Mute a remote node. Remote nodes are muted we detect a
 * loop in the mesh.
 *
 * @param node      The local node
 * @param remote    The remote node to mute
 * @param newState  Indicates if the remote is muted or dead
 *
 * @return DPS_OK if mute is successful, an error otherwise
 */
DPS_Status DPS_MuteRemoteNode(DPS_Node* node, RemoteNode* remote, RemoteNodeState newState);

/**
 * Unmute a remote node
 *
 * @param node    The local node
 * @param remote  The remote node to unmute
 *
 * @return DPS_OK if unmute is successful, an error otherwise
 */
DPS_Status DPS_UnmuteRemoteNode(DPS_Node* node, RemoteNode* remote);

/**
 * Clears the inbound interests and needs for a remote node
 * including all the count vector bookkeeping.
 *
 * @param node    The local node
 * @param remote  The remote node to clear
 */
void DPS_ClearInboundInterests(DPS_Node* node, RemoteNode* remote);

/**
 * Set outbound interests and needs to an empty bit vector
 *
 * @param remote  The remote node to clear
 *
 * @return DPS_OK if clear is successful, an error otherwise
 */
DPS_Status DPS_ClearOutboundInterests(RemoteNode* remote);

/**
 * Update outbound interests and needs
 *
 * @param node    The local node
 * @param remote  The remote node
 * @param send    TRUE if subscription should be sent
 *
 * @return DPS_OK if update is successful, an error otherwise
 */
DPS_Status DPS_UpdateOutboundInterests(DPS_Node* node, RemoteNode* remote, uint8_t* send);

/**
 * Lock the node
 *
 * @param node The node to lock
 */
void DPS_LockNode(DPS_Node* node);

/**
 * Unlock the node
 *
 * @param node The node to unlock
 */
void DPS_UnlockNode(DPS_Node* node);

/**
 * Look for node's publication matching the ID and sequence number.
 *
 * @param node The node
 * @param pubId The ID to look for
 * @param sequenceNum The sequence number to look for
 *
 * @return The matching publication or NULL
 */
DPS_Publication* DPS_LookupAckHandler(DPS_Node* node, const DPS_UUID* pubId, uint32_t sequenceNum);

/**
 * Generates a random UUID that is less than the UUID passed in.
 * Less in this context means DPS_UUIDCompare(&new, old) < 0
 *
 * @param uuidIn  The input UUID
 * @param uuidOut Returns a UUID that is less than uuidIn
 */
void DPS_RandUUIDLess(const DPS_UUID* uuidIn, DPS_UUID* uuidOut);

#ifdef __cplusplus
}
#endif

#endif

