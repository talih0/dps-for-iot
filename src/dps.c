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

#include <assert.h>
#include <math.h>
#include <safe_lib.h>
#include <stdlib.h>
#include <string.h>
#include <dps/dbg.h>
#include <dps/dps.h>
#include <dps/private/cbor.h>
#include <dps/private/dps.h>
#include <dps/uuid.h>
#include "ack.h"
#include "bitvec.h"
#include "coap.h"
#include "crypto.h"
#include "ec.h"
#include "history.h"
#include "node.h"
#include "pub.h"
#include "resolver.h"
#include "sub.h"
#include "topics.h"
#include "uv_extra.h"

#undef DPS_DBG_TAG
#define DPS_DBG_TAG ((node)->addrStr)

/*
 * Debug control for this module
 */
#if defined(DEBUG_LINK_LOSS)
DPS_DEBUG_CONTROL(DPS_DEBUG_INFO);
#else
DPS_DEBUG_CONTROL(DPS_DEBUG_ON);
#endif

#define DESCRIBE(n)  DPS_NodeAddrToString(&(n)->ep.addr)

#define _MIN_(x, y)  (((x) < (y)) ? (x) : (y))

typedef enum { NO_REQ, SUB_REQ, PUB_REQ, ACK_REQ } RequestType;

typedef enum { LINK_OP, UNLINK_OP } OpType;

typedef struct _OnOpCompletion {
    OpType op;
    void* data;
    struct _RemoteNode* remote;
    DPS_NodeAddress addr;
    DPS_Status status;
    union {
        DPS_OnLinkComplete link;
        DPS_OnUnlinkComplete unlink;
        void* cb;
    } on;
    NodeRequest req;
} OnOpCompletion;

const DPS_UUID DPS_MaxMeshId = { .val64 = { UINT64_MAX, UINT64_MAX } };

/*
 * We just need a unique pointer here to represent DPS_LoopbackNode.
 * The actual value doesn't matter since it is not a real RemoteNode.
 */
RemoteNode* DPS_LoopbackNode = (RemoteNode*)&DPS_LoopbackNode;

static void DumpNode(uv_signal_t* handle, int signum);
static void SendSubsTimer(uv_timer_t* handle);
static void SendPubsTimer(uv_timer_t* handle);
static void LinkExists(NodeRequest* req);
static DPS_Status Unlink(DPS_Node* node, RemoteNode* remote, DPS_OnUnlinkComplete cb, void* data);

void DPS_LockNode(DPS_Node* node)
{
    uv_mutex_lock(&node->nodeMutex);
#ifdef DPS_DEBUG
    ++node->isLocked;
#endif
}

void DPS_UnlockNode(DPS_Node* node)
{
#ifdef DPS_DEBUG
    assert(node->isLocked);
    --node->isLocked;
#endif
    uv_mutex_unlock(&node->nodeMutex);
}

void DPS_RxBufferFree(DPS_RxBuffer* buffer)
{
    if (buffer->base) {
        free(buffer->base);
        buffer->base = NULL;
    }
    buffer->rxPos = NULL;
    buffer->eod = NULL;
}

void DPS_TxBufferFree(DPS_TxBuffer* buffer)
{
    if (buffer->base) {
        free(buffer->base);
        buffer->base = NULL;
    }
    buffer->txPos = NULL;
    buffer->eob = NULL;
}

DPS_Status DPS_RxBufferInit(DPS_RxBuffer* buffer, uint8_t* storage, size_t size)
{
    if (!size) {
        buffer->base = NULL;
        buffer->rxPos = NULL;
        buffer->eod = NULL;
    } else {
        assert(storage);
        buffer->base = storage;
        buffer->rxPos = storage;
        buffer->eod = storage + size;
    }
    return DPS_OK;
}

DPS_Status DPS_TxBufferInit(DPS_TxBuffer* buffer, uint8_t* storage, size_t size)
{
    DPS_Status ret = DPS_OK;
    if (!storage && size) {
        storage = malloc(size);
        if (!storage) {
            ret = DPS_ERR_RESOURCES;
            size = 0;
        }
    }
    buffer->base = storage;
    buffer->txPos = storage;
    buffer->eob = storage + size;
    return ret;
}

DPS_Status DPS_TxBufferAppend(DPS_TxBuffer* buffer, const uint8_t* data, size_t len)
{
    if (data && len) {
        if (memcpy_s(buffer->txPos, DPS_TxBufferSpace(buffer), data, len) != EOK) {
            return DPS_ERR_OVERFLOW;
        }
        buffer->txPos += len;
    }
    return DPS_OK;
}

void DPS_TxBufferToRx(const DPS_TxBuffer* txBuffer, DPS_RxBuffer* rxBuffer)
{
    assert(txBuffer && rxBuffer);
    rxBuffer->base = txBuffer->base;
    rxBuffer->eod = txBuffer->txPos;
    rxBuffer->rxPos = txBuffer->base;
}

void DPS_RxBufferToTx(const DPS_RxBuffer* rxBuffer, DPS_TxBuffer* txBuffer)
{
    assert(rxBuffer && txBuffer);
    txBuffer->base = rxBuffer->base;
    txBuffer->eob = rxBuffer->eod;
    txBuffer->txPos = rxBuffer->eod;
}

static void OnShutdown(DPS_Node* node)
{
    DPS_OnNodeShutdown onShutdown = NULL;
    void* onShutdownData = NULL;

    DPS_LockNode(node);
    if (node->onShutdown && !node->remoteNodes) {
        onShutdown = node->onShutdown;
        onShutdownData = node->onShutdownData;
        node->onShutdown = node->onShutdownData = NULL;
    }
    DPS_UnlockNode(node);
    if (onShutdown) {
        DPS_DBGPRINT("OnShutdown\n");
        onShutdown(node, onShutdownData);
    }
}

static void OnShutdownRequest(NodeRequest* req)
{
    OnShutdown(req->node);
}

static void OnShutdownUnlinkComplete(DPS_Node* node, const DPS_NodeAddress* addr, void* data)
{
    OnShutdown(node);
}

static void RemoteCompletion(OnOpCompletion* completion)
{
    DPS_Node* node = completion->req.node;
    RemoteNode* remote = completion->remote;
    const DPS_NodeAddress* addr = &completion->addr;
    DPS_Status status = completion->status;
    OpType op = completion->op;

    DPS_DBGTRACEA("For %s\n", DPS_NodeAddrToString(addr));

    if (remote) {
        if (op == LINK_OP) {
            /*
             * Expected states are LINKING, ACTIVE or MUTED
             */
            if (remote->state == REMOTE_LINKING) {
                remote->state = REMOTE_ACTIVE;
            }
        } else if (op == UNLINK_OP) {
            /*
             * We cannot UNLINK while there a SAK is pending so
             * if we get here and the state is not UNLINKING it
             * means we got the SAK and can now do the unlink.
             */
            if (remote->state != REMOTE_UNLINKING) {
                uint8_t unused;
                DPS_Status ret;
                DPS_DBGPRINT("Deferred unlink for %s\n", DESCRIBE(remote));
                remote->state = REMOTE_UNLINKING;
                DPS_UpdateOutboundInterests(node, remote, &unused);
                ret = DPS_SendSubscription(node, remote);
                if (ret == DPS_OK) {
                    return;
                } else {
                    DPS_ERRPRINT("DPS_SendSubscription failed - %s\n", DPS_ErrTxt(ret));
                }
            }
            status = DPS_ERR_MISSING;
        }
        assert(remote->completion == completion);
        remote->completion = NULL;
        if ((status != DPS_OK) && (status != DPS_ERR_EXISTS)) {
            DPS_DeleteRemoteNode(node, remote);
        }
    }
    DPS_NodeRequestCancel(&completion->req);
    /* TODO callbacks are issued with the lock here */
    if (op == LINK_OP) {
        completion->on.link(node, addr, status, completion->data);
    } else if (op == UNLINK_OP) {
        completion->on.unlink(node, addr, completion->data);
    }
    free(completion);

    if (node->onShutdown) {
        /*
         * We are shutting down.  Check if this is the last remote and
         * issue the shutdown callback.
         */
        if (!remote || (op == UNLINK_OP)) {
            OnShutdown(node);
        } else {
            /*
             * A LINK_OP was in progress when DPS_ShutdownNode was
             * called so unlink here to continue the shutdown process.
             */
            status = Unlink(node, remote, OnShutdownUnlinkComplete, NULL);
            assert(status != DPS_ERR_BUSY);
            if (status != DPS_OK) {
                DPS_ERRPRINT("Unlink failed - %s\n", DPS_ErrTxt(status));
                DPS_DeleteRemoteNode(node, remote);
                OnShutdown(node);
            }
        }
    }
}

void DPS_RemoteCompletion(OnOpCompletion* completion, DPS_Status status)
{
    completion->status = status;
    RemoteCompletion(completion);
}

static int IsValidRemoteNode(DPS_Node* node, RemoteNode* remote)
{
    RemoteNode* r = node->remoteNodes;

    while (r) {
        if (r == remote) {
            return DPS_TRUE;
        }
        r = r->next;
    }

    return DPS_FALSE;
}

static void FreeOutboundInterests(RemoteNode* remote)
{
    DPS_BitVectorFree(remote->outbound.interests);
    remote->outbound.interests = NULL;
    DPS_BitVectorFree(remote->outbound.needs);
    remote->outbound.needs = NULL;
}

DPS_Status DPS_ClearOutboundInterests(RemoteNode* remote)
{
    if (remote->outbound.interests) {
        DPS_BitVectorClear(remote->outbound.interests);
    } else {
        remote->outbound.interests = DPS_BitVectorAlloc();
    }
    if (remote->outbound.needs) {
        DPS_BitVectorClear(remote->outbound.needs);
    } else {
        remote->outbound.needs = DPS_BitVectorAllocFH();
    }
    /*
     * We are clearing the interests this is not a delta
     */
    remote->outbound.deltaInd = DPS_FALSE;
    if (!remote->outbound.interests || !remote->outbound.needs) {
        FreeOutboundInterests(remote);
        return DPS_ERR_RESOURCES;
    } else {
        return DPS_OK;
    }
}

void DPS_ClearInboundInterests(DPS_Node* node, RemoteNode* remote)
{
    if (remote->inbound.interests) {
        if (DPS_CountVectorDel(node->interests, remote->inbound.interests) != DPS_OK) {
            assert(!"Count error");
        }
        DPS_BitVectorFree(remote->inbound.interests);
        remote->inbound.interests = NULL;
    }
    if (remote->inbound.needs) {
        if (DPS_CountVectorDel(node->needs, remote->inbound.needs) != DPS_OK) {
            assert(!"Count error");
        }
        DPS_BitVectorFree(remote->inbound.needs);
        remote->inbound.needs = NULL;
    }
}

static void InsertRemoteNode(DPS_Node* node, RemoteNode* remote)
{
    remote->next = node->remoteNodes;
    node->remoteNodes = remote;
}

static void RemoveRemoteNode(DPS_Node* node, RemoteNode* remote)
{
    RemoteNode* prev = node->remoteNodes;
    if (prev == remote) {
        node->remoteNodes = remote->next;
    } else {
        while (prev->next != remote) {
            prev = prev->next;
            assert(prev);
        }
        prev->next = remote->next;
    }
    remote->next = NULL;
}

void DPS_DeleteRemoteNode(DPS_Node* node, RemoteNode* remote)
{
    if (!IsValidRemoteNode(node, remote)) {
        DPS_WARNPRINT("Attempt to delete invalid remote %p\n", remote);
        return;
    }

    DPS_DBGTRACEA("%s\n", DESCRIBE(remote));

    RemoveRemoteNode(node, remote);
    DPS_ClearInboundInterests(node, remote);
    FreeOutboundInterests(remote);
    DPS_BitVectorFree(remote->outbound.delta);

    if (remote->completion) {
        /*
         * Clear the remote so DPS_RemoteCompletion() won't try to delete it.
         */
        remote->completion->remote = NULL;
        DPS_RemoteCompletion(remote->completion, DPS_ERR_MISSING);
    } else if (remote->outbound.linkRequested && node->linkLossCB) {
        node->linkLossCB(node, &remote->ep.addr, node->linkLossData);
    }
    DPS_NetConnectionDecRef(remote->ep.cn);
    free(remote);
}

const DPS_UUID* DPS_MinMeshId(const DPS_Node* node, const RemoteNode* excluded)
{
    RemoteNode* remote;
    const DPS_UUID* minMeshId = &node->meshId;

    for (remote = node->remoteNodes; remote != NULL; remote = remote->next) {
        if (remote == excluded || (remote->state != REMOTE_ACTIVE && remote->state != REMOTE_LINKING)) {
            continue;
        }
        if (DPS_UUIDCompare(&remote->inbound.meshId, minMeshId) < 0) {
            minMeshId = &remote->inbound.meshId;
        }
    }
    return minMeshId;
}

DPS_Status DPS_UpdateOutboundInterests(DPS_Node* node, RemoteNode* destNode, uint8_t* changes)
{
    DPS_Status ret;
    DPS_BitVector* newInterests = NULL;
    DPS_BitVector* newNeeds = NULL;

    DPS_DBGTRACE();

    /*
     * Unlinking is always a change
     */
    if (destNode->state == REMOTE_UNLINKING) {
        FreeOutboundInterests(destNode);
        ++destNode->outbound.revision;
        *changes = DPS_TRUE;
        return DPS_OK;
    }
    *changes = DPS_FALSE;
    /*
     * Inbound interests from the node we are updating are excluded from the
     * recalculation of outbound interests
     */
    if (destNode->inbound.interests) {
        ret = DPS_CountVectorDel(node->interests, destNode->inbound.interests);
        if (ret != DPS_OK) {
            goto ErrExit;
        }
        newInterests = DPS_CountVectorToUnion(node->interests);
        ret = DPS_CountVectorAdd(node->interests, destNode->inbound.interests);
        if (ret != DPS_OK) {
            goto ErrExit;
        }
        ret = DPS_CountVectorDel(node->needs, destNode->inbound.needs);
        if (ret != DPS_OK) {
            goto ErrExit;
        }
        newNeeds = DPS_CountVectorToIntersection(node->needs);
        ret = DPS_CountVectorAdd(node->needs, destNode->inbound.needs);
        if (ret != DPS_OK) {
            goto ErrExit;
        }
    } else {
        assert(!destNode->inbound.needs);
        newInterests = DPS_CountVectorToUnion(node->interests);
        newNeeds = DPS_CountVectorToIntersection(node->needs);
    }
    if (!newNeeds || !newInterests) {
        ret = DPS_ERR_RESOURCES;
        goto ErrExit;
    }
    /*
     * Send a delta if we have previously sent interests. The needs vector
     * is small and typically dense so it is not worth computing a delta.
     */
    if (destNode->outbound.interests) {
        int same = DPS_FALSE;
        if (!destNode->outbound.delta) {
            destNode->outbound.delta = DPS_BitVectorAlloc();
            if (!destNode->outbound.delta) {
                ret = DPS_ERR_RESOURCES;
                goto ErrExit;
            }
        }
        DPS_BitVectorXor(destNode->outbound.delta, destNode->outbound.interests, newInterests, &same);
        if (same) {
            if (!DPS_BitVectorEquals(destNode->outbound.needs, newNeeds)) {
                DPS_ERRPRINT("Inconsistency between interests and needs for %s\n", DESCRIBE(destNode));
                assert(DPS_BitVectorEquals(destNode->outbound.needs, newNeeds));
            }
        } else {
            *changes = DPS_TRUE;
        }
        destNode->outbound.deltaInd = DPS_TRUE;
    } else {
        /*
         * This is not a delta
         */
        destNode->outbound.deltaInd = DPS_FALSE;
        *changes = DPS_TRUE;
    }
    FreeOutboundInterests(destNode);
    destNode->outbound.interests = newInterests;
    destNode->outbound.needs = newNeeds;

    /*
     * Increment the revision number if there were changes
     */
    if (*changes) {
        ++destNode->outbound.revision;
        DPS_DBGPRINT("New outbound interests[%d] for %s: %s\n", destNode->outbound.revision,
                DESCRIBE(destNode), DPS_DumpMatchingTopics(destNode->outbound.interests));
    }
    return DPS_OK;

ErrExit:
    DPS_ERRPRINT("DPS_UpdateOutboundInterests: %s\n", DPS_ErrTxt(ret));

    DPS_BitVectorFree(newInterests);
    DPS_BitVectorFree(newNeeds);
    return ret;
}

DPS_Status DPS_MuteRemoteNode(DPS_Node* node, RemoteNode* remote, RemoteNodeState newState)
{
    if (remote->state == REMOTE_MUTED || remote->state == REMOTE_UNLINKING) {
        return DPS_OK;
    }
    remote->state = newState;
    DPS_DBGPRINT("%s %s\n", RemoteStateTxt(remote), DESCRIBE(remote));
    /*
     * Free the outbound interests, they are not needed while the remote is muted
     */
    FreeOutboundInterests(remote);
    remote->outbound.sendInterests = DPS_FALSE;
    /*
     * Move the muted node to the head of the remote node list so remotes
     * can be unmuted in FILO order.
     */
    RemoveRemoteNode(node, remote);
    InsertRemoteNode(node, remote);
    return DPS_OK;
}

DPS_Status DPS_UnmuteRemoteNode(DPS_Node* node, RemoteNode* remote)
{
    DPS_Status ret = DPS_OK;

    DPS_DBGTRACE();

    if (remote->state == REMOTE_MUTED || remote->state == REMOTE_UNMUTING) {
        uint8_t unused;

        DPS_DBGPRINT("Unmuting %s remote %s\n", RemoteStateTxt(remote), DESCRIBE(remote));

        remote->state = REMOTE_UNMUTING;
        remote->outbound.sakCounter = 0;
        /*
         * We need a fresh mesh id that is less than any of the mesh id's
         * we have already seen. If we were to send the same mesh id that
         * was used to detect the loop it will look to the remaining
         * nodes like there is still a loop.
         */
        DPS_RandUUIDLess(DPS_MinMeshId(node, NULL), &node->meshId);
        /*
         * We didn't send any interests out on this link while is was
         * muted so we need to bring the remote up to date. First clear
         * the interests because we don't want to send a delta.
         */
        DPS_ClearOutboundInterests(remote);
        ret = DPS_UpdateOutboundInterests(node, remote, &unused);
        if (ret == DPS_OK) {
            ret = DPS_SendSubscription(node, remote);
        }
    }
    return ret;
}

RemoteNode* DPS_LookupRemoteNode(DPS_Node* node, const DPS_NodeAddress* addr)
{
    RemoteNode* remote;

    if (!addr) {
        return NULL;
    }
    for (remote = node->remoteNodes; remote != NULL; remote = remote->next) {
        if (DPS_SameAddr(&remote->ep.addr, addr)) {
            return remote;
        }
    }
    return NULL;
}

static OnOpCompletion* AllocCompletion(DPS_Node* node, RemoteNode* remote, OpType op, void* data, void* cb)
{
    OnOpCompletion* cpn;

    cpn = calloc(1, sizeof(OnOpCompletion));
    if (cpn) {
        cpn->op = op;
        cpn->data = data;
        cpn->remote = remote;
        cpn->on.cb = cb;
        DPS_NodeRequestInit(node, &cpn->req, LinkExists);
        cpn->req.data = cpn;
    }
    return cpn;
}

/*
 * Add a remote node or return an existing one
 */
DPS_Status DPS_AddRemoteNode(DPS_Node* node, const DPS_NodeAddress* addr, DPS_NetConnection* cn, RemoteNode** remoteOut)
{
    DPS_Status ret = DPS_OK;
    RemoteNode* remote = DPS_LookupRemoteNode(node, addr);

    if (remote) {
        if (!remote->ep.cn) {
            DPS_NetConnectionIncRef(cn);
            remote->ep.cn = cn;
        }
        ret = DPS_ERR_EXISTS;
    } else {
        assert(addr->type);
        remote = calloc(1, sizeof(RemoteNode));
        if (!remote) {
            *remoteOut = NULL;
            return DPS_ERR_RESOURCES;
        }
        DPS_DBGPRINT("Adding new remote %s\n", DPS_NodeAddrToString(addr));
        remote->ep.addr = *addr;
        remote->ep.cn = cn;
        DPS_NetConnectionIncRef(cn);
        remote->inbound.meshId = DPS_MaxMeshId;
        /*
         * Add remote node to the remote node list
         */
        InsertRemoteNode(node, remote);
    }
    *remoteOut = remote;
    return ret;
}

void DPS_SendComplete(DPS_Node* node, DPS_NodeAddress* addr, uv_buf_t* bufs, size_t numBufs, DPS_Status status)
{
    RemoteNode* remote;

    DPS_DBGPRINT("Send complete %s\n", DPS_ErrTxt(status));

    if (addr && (status != DPS_OK)) {
        remote = DPS_LookupRemoteNode(node, addr);
        if (remote) {
            DPS_DBGPRINT("Removing node %s\n", DPS_NodeAddrToString(addr));
            DPS_DeleteRemoteNode(node, remote);
        }
    }
    DPS_NetFreeBufs(bufs, numBufs);
}

void DPS_OnSendComplete(DPS_Node* node, void* appCtx, DPS_NetEndpoint* ep, uv_buf_t* bufs, size_t numBufs, DPS_Status status)
{
    DPS_LockNode(node);
    DPS_SendComplete(node, ep ? &ep->addr : NULL, bufs, numBufs, status);
    DPS_UnlockNode(node);
}

void DPS_OnSendSubscriptionComplete(DPS_Node* node, void* appCtx, DPS_NetEndpoint* ep, uv_buf_t* bufs, size_t numBufs, DPS_Status status)
{
    RemoteNode* remote;

    DPS_DBGPRINT("Send SUB complete %s\n", DPS_ErrTxt(status));
    if (ep) {
        DPS_LockNode(node);
        remote = DPS_LookupRemoteNode(node, &ep->addr);
        if (remote) {
            if (status == DPS_OK) {
                if ((node->state == DPS_NODE_RUNNING) && remote->outbound.sakPending) {
                    DPS_UpdateSubs(node, SubsThrottled);
                }
            } else {
                DPS_DBGPRINT("Removing node %s\n", DPS_NodeAddrToString(&ep->addr));
                DPS_DeleteRemoteNode(node, remote);
            }
        }
        DPS_UnlockNode(node);
    }
    DPS_NetFreeBufs(bufs, numBufs);
}

static void SendAcksTask(uv_async_t* handle)
{
    DPS_Node* node = (DPS_Node*)handle->data;
    PublicationAck* ack;
    RemoteNode* ackNode;
    DPS_Status ret;

    DPS_DBGTRACE();

    DPS_LockNode(node);
    while (!DPS_QueueEmpty(&node->ackQueue)) {
        ack = (PublicationAck*)DPS_QueueFront(&node->ackQueue);
        DPS_QueueRemove(&ack->queue);
        if (node->state == DPS_NODE_RUNNING) {
            /*
             * Send the acknowledgement through an existing connection
             * when available, otherwise use the destination address
             * directly.
             */
            ackNode = DPS_LookupRemoteNode(node, &ack->destAddr);
            if (ackNode) {
                ret = DPS_SendAcknowledgement(ack, &ackNode->ep);
            } else {
                DPS_NetEndpoint ep = { ack->destAddr, NULL };
                ret = DPS_SendAcknowledgement(ack, &ep);
            }
        } else {
            ret = DPS_ERR_NOT_STARTED;
        }
        if (ret != DPS_OK) {
            DPS_AckPublicationCompletion(ack);
        }
    }
    DPS_UnlockNode(node);
}

static void PublishCompletion(DPS_PublishRequest* req)
{
    if (req) {
        assert(req->refCount > 0);
        --req->refCount;
        DPS_PublishCompletion(req);
    }
}

static void SendPubs(DPS_Node* node)
{
    DPS_Publication* pub;
    DPS_Publication* nextPub;
    DPS_Subscription* sub;
    RemoteNode* remote;
    RemoteNode* nextRemote;
    DPS_Status ret = DPS_OK;
    DPS_PublishRequest* req;
    DPS_PublishRequest* expired;
    uint64_t now;
    uint64_t reschedule = UINT64_MAX;

    DPS_LockNode(node);
    now = uv_now(node->loop);
    /*
     * Check if any local or retained publications need to be forwarded to this subscriber
     */
    for (pub = node->publications; pub != NULL; pub = nextPub) {
        nextPub = pub->next;
        DPS_PublicationIncRef(pub);
        expired = NULL;
        while (!DPS_QueueEmpty(&pub->sendQueue)) {
            req = (DPS_PublishRequest*)DPS_QueueFront(&pub->sendQueue);
            DPS_QueueRemove(&req->queue);
            assert(req->refCount > 0);
            --req->refCount;
            if (!req->expires) {
                req->expires = now + DPS_SECS_TO_MS(req->ttl);
            }
            if (pub->flags & PUB_FLAG_LOCAL) {
                /*
                 * Loopback publication if there is a matching subscriber candidate on
                 * this node
                 */
                for (sub = node->subscriptions; sub != NULL; sub = sub->next) {
                    if (DPS_BitVectorIncludes(pub->bf, sub->bf)) {
                        ret = DPS_SendPublication(req, pub, DPS_LoopbackNode);
                        if (ret != DPS_OK) {
                            DPS_ERRPRINT("SendPublication (loopback) returned %s\n", DPS_ErrTxt(ret));
                        }
                        break;
                    }
                }
                /*
                 * If the node is a multicast sender local publications may be multicast
                 */
                if (node->mcastSender && (pub->flags & PUB_FLAG_MULTICAST)) {
                    ret = DPS_SendPublication(req, pub, NULL);
                    if (ret != DPS_OK) {
                        DPS_ERRPRINT("SendPublication (multicast) returned %s\n", DPS_ErrTxt(ret));
                    }
                }
            }
            for (remote = node->remoteNodes; remote != NULL; remote = nextRemote) {
                nextRemote = remote->next;
                DPS_DBGPRINT("%s %s interests=%p\n", DESCRIBE(remote), RemoteStateTxt(remote), remote->inbound.interests);
                if (remote->state != REMOTE_ACTIVE || !remote->inbound.interests) {
                    continue;
                }
                if (!(pub->flags & PUB_FLAG_LOCAL)) {
                    /*
                     * We don't send publications to remote nodes we have received them from.
                     */
                    if (DPS_PublicationReceivedFrom(&node->history, &pub->pubId, req->sequenceNum,
                                &pub->senderAddr, &remote->ep.addr)) {
                        continue;
                    }
                }
                /*
                 * This is the pub/sub matching code
                 */
                DPS_BitVectorIntersection(node->scratch.interests, pub->bf, remote->inbound.interests);
                DPS_BitVectorFuzzyHash(node->scratch.needs, node->scratch.interests);
                if (!DPS_BitVectorIncludes(node->scratch.needs, remote->inbound.needs)) {
                    DPS_DBGPRINT("Rejected pub %s(%d) for %s: %s\n", DPS_UUIDToString(&pub->pubId),
                                 req->sequenceNum, DESCRIBE(remote),
                                 DPS_DumpMatchingTopics(remote->inbound.interests));
                    continue;
                }
                DPS_DBGPRINT("Sending pub %s(%d) to %s\n", DPS_UUIDToString(&pub->pubId),
                             req->sequenceNum, DESCRIBE(remote));
                ret = DPS_SendPublication(req, pub, remote);
                if (ret != DPS_OK) {
                    DPS_DeleteRemoteNode(node, remote);
                    DPS_ERRPRINT("SendPublication (unicast) returned %s\n", DPS_ErrTxt(ret));
                }
            }
            if (!DPS_QueueEmpty(&pub->retainedQueue)) {
                PublishCompletion(expired);
                expired = (DPS_PublishRequest*)DPS_QueueFront(&pub->retainedQueue);
                DPS_QueueRemove(&expired->queue);
            }
            if (now < req->expires) {
                DPS_QueuePushBack(&pub->retainedQueue, &req->queue);
                ++req->refCount;
                reschedule = (req->expires < reschedule) ? req->expires : reschedule;
            }
            DPS_PublishCompletion(req);
        }
        if (!DPS_QueueEmpty(&pub->retainedQueue)) {
            DPS_PublishRequest* retained = (DPS_PublishRequest*)DPS_QueueFront(&pub->retainedQueue);
            if (retained->expires <= now) {
                PublishCompletion(expired);
                expired = (DPS_PublishRequest*)DPS_QueueFront(&pub->retainedQueue);
                DPS_QueueRemove(&expired->queue);
            }
        }
        if (DPS_QueueEmpty(&pub->retainedQueue)) {
            if (expired && ((pub->flags & PUB_FLAG_EXPIRED) == 0)) {
                pub->flags |= PUB_FLAG_EXPIRED;
                pub->ttl = expired->ttl = -1;
                DPS_CallPubHandlers(expired);
            }
            DPS_ExpirePub(node, pub);
        }
        PublishCompletion(expired);
        DPS_PublicationDecRef(pub);
    }
    DPS_DumpPubs(node);
    if (reschedule < UINT64_MAX) {
        uv_timer_start(&node->pubsTimer, SendPubsTimer, (reschedule < now) ? 0 : (reschedule - now), 0);
    }
    DPS_UnlockNode(node);
}

static void SendPubsTask(uv_async_t* handle)
{
    SendPubs(handle->data);
}

static void SendPubsTimer(uv_timer_t* handle)
{
    SendPubs(handle->data);
}

/*
 * See if there is a muted remote with a given same mesh id
 * that can restore connectivity after a node has gone unresponsive.
 */
static void FindAlternativeRoute(DPS_Node* node, RemoteNode* oldRoute)
{
    RemoteNode* remote;
    RemoteNode* altRoute = NULL;

    DPS_DBGINFO("Find alternative route to replace %s\n", DESCRIBE(oldRoute));

    /*
     * Prefer a mesh id match if there is one otherwise use the first one
     */
    for (remote = node->remoteNodes; remote != NULL; remote = remote->next) {
        if (remote == oldRoute) {
            continue;
        }
        if (DPS_UUIDCompare(&remote->inbound.meshId, &oldRoute->inbound.meshId) == 0) {
            if (remote->state == REMOTE_ACTIVE || remote->state == REMOTE_UNMUTING) {
                /*
                 * Route already exists or is already being restored
                 */
                return;
            }
            if (remote->state == REMOTE_MUTED) {
                altRoute = remote;
                break;
            }
        }
        if (!altRoute && remote->state == REMOTE_MUTED)  {
            altRoute = remote;
        }
    }
    if (altRoute) {
        DPS_DBGINFO("Try unmuting link to %s mesh id %s\n", DESCRIBE(altRoute), DPS_UUIDToString(&altRoute->inbound.meshId));
        DPS_UnmuteRemoteNode(node, altRoute);
    } else {
        DPS_WARNPRINT("No alternative route to replace %s\n", DESCRIBE(oldRoute));
    }
}

static void SendSubsTimer(uv_timer_t* handle)
{
    DPS_Node* node = (DPS_Node*)handle->data;
    DPS_Status ret = DPS_OK;
    RemoteNode* remote;
    RemoteNode* remoteNext = NULL;
    int reschedule = DPS_FALSE;

    DPS_DBGTRACE();

    DPS_LockNode(node);
    /*
     * Evaluate if subscriptions should be sent to remote nodes. Subscriptions messages
     * will be sent if there have been changes that need to propagate through the mesh
     * or as a periodic check that active remote nodes are still responsive.
     */
    for (remote = node->remoteNodes; remote != NULL; remote = remoteNext) {
        uint8_t changes = DPS_FALSE;
        remoteNext = remote->next; /* remote may get deleted or moved */
        /*
         * Resend the previous SUB or SAK if it has not been ACK'd
         */
        if (remote->outbound.sakPending) {
            /*
             * This is a check that time has elapsed so we don't resend SUBs too early
             */
            if (node->subsPending == SubsSendNow) {
                reschedule = DPS_TRUE;
                continue;
            }
            /*
             * We don't resend until we hit the retry threshold
             */
            if (++remote->outbound.sakCounter < DPS_SAK_RETRY_THRESHOLD) {
                reschedule = DPS_TRUE;
                continue;
            }
            if (remote->outbound.sakCounter >= (DPS_SAK_RETRY_THRESHOLD + DPS_SAK_RETRY_LIMIT)) {
                DPS_DBGINFO("At retry limit - deleting remote %s\n", DESCRIBE(remote));
                if (remote->state == REMOTE_ACTIVE) {
                    /*
                     * Check is there is a muted remote that can replace the one that was lost
                     */
                    FindAlternativeRoute(node, remote);
                }
                DPS_DeleteRemoteNode(node, remote);
                continue;
            }
            /*
             * Resend a SUB or SAK
             */
            DPS_DBGINFO("Resend(%d) %s to %s\n", remote->outbound.sakCounter - DPS_SAK_RETRY_THRESHOLD,
                    remote->outbound.lastSubMsgType == DPS_MSG_TYPE_SAK ? "SAK" : "SUB", DESCRIBE(remote));

            if (remote->outbound.lastSubMsgType == DPS_MSG_TYPE_SUB) {
                ret = DPS_SendSubscription(node, remote);
            } else {
                ret = DPS_SendSubscriptionAck(node, remote, DPS_FALSE);
            }
            if (ret == DPS_OK) {
                reschedule = DPS_TRUE;
            }
        } else {
            /*
             * Ignore muted links
             */
            if (remote->state == REMOTE_MUTED) {
                continue;
            }
            ret = DPS_UpdateOutboundInterests(node, remote, &changes);
            if (ret == DPS_OK) {
                /*
                 * If subsPending == SubsNothingPending it means the keep alive timer
                 * triggered so send subscription message even when there are no changes.
                 */
                if (changes || node->subsPending == SubsNonePending) {
                    ret = DPS_SendSubscription(node, remote);
                    if (ret == DPS_OK) {
                        reschedule = DPS_TRUE;
                    }
                }
            }
        }
        if (ret != DPS_OK) {
            DPS_WARNPRINT("Failed to send subscription request %s\n", DPS_ErrTxt(ret));
            ret = DPS_OK;
            DPS_DeleteRemoteNode(node, remote);
        }
    }
    if (ret != DPS_OK) {
        DPS_ERRPRINT("SendSubsTimer failed %s\n", DPS_ErrTxt(ret));
    }
    if (reschedule) {
        node->subsPending = SubsThrottled;
        uv_timer_start(&node->subsTimer, SendSubsTimer, node->subsRate, node->linkLossTimeout);
    } else {
        node->subsPending = SubsNonePending;
    }
    DPS_UnlockNode(node);
}

static void SendSubsTask(uv_async_t* handle)
{
    DPS_Node* node = (DPS_Node*)handle->data;

    DPS_LockNode(node);
    if (node->state == DPS_NODE_RUNNING) {
        if (node->subsPending == SubsSendNow) {
            SendSubsTimer(&node->subsTimer);
        } else {
            uv_timer_start(&node->subsTimer, SendSubsTimer, node->subsRate, node->linkLossTimeout);
        }
    }
    DPS_UnlockNode(node);
}

/*
 * Run checks of the publications against the current subscriptions
 */
void DPS_UpdatePubs(DPS_Node* node)
{
    int count = 0;
    DPS_Publication* pub;
    DPS_Publication* nextPub;
    DPS_PublishRequest* req;

    DPS_DBGTRACE();

#ifdef DPS_DEBUG
    assert(node->isLocked);
#endif
    if (node->state != DPS_NODE_RUNNING) {
        return;
    }
    for (pub = node->publications; pub != NULL; pub = nextPub) {
        nextPub = pub->next;
        if (!DPS_QueueEmpty(&pub->sendQueue)) {
            ++count;
        } else if (DPS_QueueEmpty(&pub->retainedQueue)) {
            DPS_ExpirePub(node, pub);
        } else if (node->remoteNodes || node->mcastSender) {
            req = (DPS_PublishRequest*)DPS_QueueFront(&pub->retainedQueue);
            DPS_QueueRemove(&req->queue);
            DPS_QueuePushBack(&pub->sendQueue, &req->queue);
            ++count;
        }
    }
    if (count) {
        DPS_DBGPRINT("DPS_UpdatePubs %d publications to send\n", count);
        uv_async_send(&node->pubsAsync);
    }
}

void DPS_UpdateSubs(DPS_Node* node, SubsPendingState pending)
{
#ifdef DPS_DEBUG
    assert(node->isLocked);
#endif
    if (node->state == DPS_NODE_RUNNING) {
        if (node->subsPending != SubsSendNow && pending == SubsThrottled) {
            node->subsPending = SubsThrottled;
        } else {
            node->subsPending = SubsSendNow;
        }
        uv_async_send(&node->subsAsync);
    }
}

void DPS_QueuePublicationAck(DPS_Node* node, PublicationAck* ack)
{
    DPS_DBGTRACE();

    DPS_LockNode(node);
    DPS_QueuePushBack(&node->ackQueue, &ack->queue);
    uv_async_send(&node->acksAsync);
    DPS_UnlockNode(node);
}

static DPS_Status DecodeRequest(DPS_Node* node, DPS_NetEndpoint* ep, DPS_NetRxBuffer* buf, int multicast)
{
    DPS_RxBuffer* rxBuf = (DPS_RxBuffer*)buf;
    DPS_Status ret;
    uint8_t msgVersion;
    uint8_t msgType;
    size_t len;

    if (node->state == DPS_NODE_PAUSED) {
        return DPS_OK;
    }
    DPS_DBGTRACEA("node=%p,ep={addr=%s,cn=%p},buf=%p,multicast=%d\n",
            node, DPS_NodeAddrToString(&ep->addr), ep->cn, buf, multicast);

    CBOR_Dump("Request in", rxBuf->rxPos, DPS_RxBufferAvail(rxBuf));
    ret = CBOR_DecodeArray(rxBuf, &len);
    if (ret != DPS_OK || (len != 5)) {
        DPS_ERRPRINT("Expected a CBOR array of 5 elements\n");
        return ret;
    }
    ret = CBOR_DecodeUint8(rxBuf, &msgVersion);
    if (ret != DPS_OK) {
        DPS_ERRPRINT("Expected a message type\n");
        return ret;
    }
    if (msgVersion != DPS_MSG_VERSION) {
        DPS_ERRPRINT("Expected message version %d, received %d\n", DPS_MSG_VERSION, msgVersion);
        return DPS_ERR_NOT_IMPLEMENTED;
    }
    ret = CBOR_DecodeUint8(rxBuf, &msgType);
    if (ret != DPS_OK) {
        DPS_ERRPRINT("Expected a message type\n");
        return ret;
    }
    ret = DPS_ERR_INVALID;
    switch (msgType) {
    case DPS_MSG_TYPE_SUB:
        ret = DPS_DecodeSubscription(node, ep, buf);
        if (ret != DPS_OK) {
            DPS_DBGPRINT("DecodeSubscription returned %s\n", DPS_ErrTxt(ret));
        }
        break;
    case DPS_MSG_TYPE_PUB:
        ret = DPS_DecodePublication(node, ep, buf, multicast);
        if (ret != DPS_OK) {
            DPS_DBGPRINT("DecodePublication returned %s\n", DPS_ErrTxt(ret));
        }
        break;
    case DPS_MSG_TYPE_ACK:
        ret = DPS_DecodeAcknowledgement(node, ep, buf);
        if (ret != DPS_OK) {
            DPS_DBGPRINT("DPS_DecodeAcknowledgement returned %s\n", DPS_ErrTxt(ret));
        }
        break;
    case DPS_MSG_TYPE_SAK:
        ret = DPS_DecodeSubscriptionAck(node, ep, buf);
        if (ret != DPS_OK) {
            DPS_DBGPRINT("DPS_DecodeSubscriptionAck returned %s\n", DPS_ErrTxt(ret));
        }
        break;
    default:
        DPS_ERRPRINT("Invalid message type\n");
        break;
    }
    /*
     * Stale is not a network or invalid message error, so don't
     * report an error to the transport.
     */
    if (ret == DPS_ERR_STALE) {
        ret = DPS_OK;
    }
    return ret;
}

/*
 * Using CoAP packetization for receiving multicast subscription requests
 */
static DPS_Status OnMulticastReceive(DPS_Node* node, DPS_NetEndpoint* ep, DPS_Status status,
        DPS_NetRxBuffer* buf)
{
    DPS_Status ret;
    CoAP_Parsed coap;

    DPS_DBGTRACE();

    /*
     * Fail input that comes in when the node is no longer running
     */
    DPS_LockNode(node);
    if (node->state != DPS_NODE_RUNNING) {
        DPS_UnlockNode(node);
        return DPS_ERR_FAILURE;
    }
    DPS_UnlockNode(node);

    memset(&coap, 0, sizeof(coap));
    ret = CoAP_Parse(&buf->rx, &coap);
    if (ret != DPS_OK) {
        DPS_ERRPRINT("Discarding garbage multicast packet len=%zu\n", buf->rx.eod - buf->rx.base);
        goto Exit;
    }
    /*
     * Multicast packets must be non-confirmable
     */
    if (coap.type != COAP_TYPE_NON_CONFIRMABLE) {
        DPS_ERRPRINT("Discarding packet within bad type=%d\n", coap.type);
        ret = DPS_ERR_INVALID;
        goto Exit;
    }
    ret = DecodeRequest(node, ep, buf, DPS_TRUE);
Exit:
    CoAP_Free(&coap);
    return ret;
}

static DPS_Status OnNetReceive(DPS_Node* node, DPS_NetEndpoint* ep, DPS_Status status, DPS_NetRxBuffer* buf)
{
    DPS_DBGTRACEA("node=%p,ep={addr=%s,cn=%p},status=%s,buf=%p\n", node, DPS_NodeAddrToString(&ep->addr),
            ep->cn, DPS_ErrTxt(status), buf);

    /*
     * Fail input that comes in when the node is no longer running
     */
    DPS_LockNode(node);
    if (node->state != DPS_NODE_RUNNING) {
        DPS_UnlockNode(node);
        return DPS_ERR_FAILURE;
    }
    DPS_UnlockNode(node);
    /*
     * Delete the remote node if the receive failed
     */
    if (status != DPS_OK) {
        RemoteNode* remote;
        DPS_LockNode(node);
        remote = DPS_LookupRemoteNode(node, &ep->addr);
        if (remote) {
            DPS_DeleteRemoteNode(node, remote);
        }
        DPS_UnlockNode(node);
        return status;
    }
    return DecodeRequest(node, ep, buf, DPS_FALSE);
}

DPS_Status DPS_LoopbackSend(DPS_Node* node, uv_buf_t* bufs, size_t numBufs)
{
    DPS_Status ret;
    DPS_NetEndpoint ep;
    DPS_NetRxBuffer* buf = NULL;
    size_t len = 0;
    size_t i;

    ret = DPS_GetLoopbackAddress(&ep.addr, node);
    if (ret != DPS_OK) {
        goto Exit;
    }
    ep.cn = NULL;

    /*
     * TODO DecodeRequest expects a contiguous buffer, so no choice
     * except to copy.
     */
    for (i = 0; i < numBufs; ++i) {
        len += bufs[i].len;
    }
    buf = DPS_CreateNetRxBuffer(len);
    if (!buf) {
        ret = DPS_ERR_RESOURCES;
        DPS_ERRPRINT("DPS_TxBufferInit failed - %s\n", DPS_ErrTxt(ret));
        goto Exit;
    }
    for (i = 0; i < numBufs; ++i) {
        if (bufs[i].len) {
            memcpy(buf->rx.rxPos, bufs[i].base, bufs[i].len);
            buf->rx.rxPos += bufs[i].len;
        }
    }
    buf->rx.rxPos = buf->rx.base;
    ret = DecodeRequest(node, &ep, buf, DPS_FALSE);

Exit:
    DPS_NetRxBufferDecRef(buf);
    return ret;
}

static void FreeTask(uv_async_t* handle)
{
    DPS_Node* node = (DPS_Node*)handle->data;
    DPS_Subscription** sub;
    DPS_Publication** pub;

    DPS_LockNode(node);
    sub = &node->freeSubs;
    while (*sub) {
        if ((*sub)->refCount == 0) {
            *sub = DPS_FreeSubscription(*sub);
        } else {
            sub = &(*sub)->next;
        }
    }
    pub = &node->freePubs;
    while (*pub) {
        if ((*pub)->refCount == 0) {
            *pub = DPS_FreePublication(*pub);
        } else {
            pub = &(*pub)->next;
        }
    }
    DPS_UnlockNode(node);
}

static void StopNode(DPS_Node* node)
{
    PublicationAck* ack;
    NodeRequest* request;

    /*
     * Indicates the node is no longer running
     */
    node->state = DPS_NODE_STOPPED;
    /*
     * Stop receiving and close all global handles
     */
    if (node->mcastReceiver) {
        DPS_MulticastStopReceive(node->mcastReceiver);
        node->mcastReceiver = NULL;
    }
    if (node->mcastSender) {
        DPS_MulticastStopSend(node->mcastSender);
        node->mcastSender = NULL;
    }
    if (node->netCtx) {
        DPS_NetStop(node->netCtx);
        node->netCtx = NULL;
    }
    assert(!uv_is_closing((uv_handle_t*)&node->stopAsync));
    uv_close((uv_handle_t*)&node->stopAsync, NULL);
    uv_close((uv_handle_t*)&node->pubsAsync, NULL);
    uv_close((uv_handle_t*)&node->pubsTimer, NULL);
    uv_close((uv_handle_t*)&node->subsAsync, NULL);
    uv_close((uv_handle_t*)&node->acksAsync, NULL);
    uv_close((uv_handle_t*)&node->subsTimer, NULL);
    uv_close((uv_handle_t*)&node->sigusr1, NULL);
    uv_close((uv_handle_t*)&node->requestAsync, NULL);
    /*
     * Cleanup any unresolved resolvers before closing the handle
     */
    DPS_AsyncResolveAddress(&node->resolverAsync);
    uv_close((uv_handle_t*)&node->resolverAsync, NULL);
    /*
     * Delete remote nodes and shutdown any connections.
     */
    while (node->remoteNodes) {
        /*
         * Clear so delete doesn't report a surprise link loss.
         */
        node->remoteNodes->outbound.linkRequested = DPS_FALSE;
        DPS_DeleteRemoteNode(node, node->remoteNodes);
    }
    /*
     * Cleanup any unresolved acks
     */
    while (!DPS_QueueEmpty(&node->ackQueue)) {
        ack = (PublicationAck*)DPS_QueueFront(&node->ackQueue);
        DPS_QueueRemove(&ack->queue);
        ack->status = DPS_ERR_WRITE;
        DPS_AckPublicationCompletion(ack);
    }
    /*
     * Cleanup any unresolved requests
     */
    while (!DPS_QueueEmpty(&node->requestQueue)) {
        request = (NodeRequest*)DPS_QueueFront(&node->requestQueue);
        DPS_QueueRemove(&request->queue);
        request->cb(request);
    }
    /*
     * Cleanup any subscriptions or publications that may have a
     * destroyed callback
     */
    DPS_FreeSubscriptions(node);
    DPS_FreePublications(node);
    /*
     * Run the event loop to ensure all cleanup is completed
     *
     * This is broken into multiple steps as freeAsync must be closed
     * before UV_RUN_DEFAULT will return, and UV_RUN_DEFAULT may
     * schedule more work for FreeTask
     */
    while (node->publications || node->freePubs || node->subscriptions || node->freeSubs) {
        uv_run(node->loop, UV_RUN_ONCE);
    }
    uv_close((uv_handle_t*)&node->freeAsync, NULL);
    uv_run(node->loop, UV_RUN_DEFAULT);
    FreeTask(&node->freeAsync);
    /*
     * Free data structures
     */
    DPS_CountVectorFree(node->interests);
    DPS_CountVectorFree(node->needs);
    DPS_BitVectorFree(node->scratch.interests);
    DPS_BitVectorFree(node->scratch.needs);
    DPS_HistoryFree(&node->history);
    /*
     * Cleanup mutexes etc.
     */
    uv_mutex_destroy(&node->condMutex);
    uv_mutex_destroy(&node->history.lock);

    assert(!uv_loop_alive(node->loop));

    uv_loop_close(node->loop);
    free(node->loop);
    node->loop = NULL;
}

static void FreeNode(DPS_Node* node)
{
    DPS_ClearKeyId(&node->signer.kid);
    DPS_DestroyRBG(node->rbg);
    free(node);
}

static void NodeRun(void* arg)
{
    int r;
    DPS_Node* node = (DPS_Node*)arg;
    uv_thread_t thisThread = node->thread;

    uv_run(node->loop, UV_RUN_DEFAULT);

    DPS_DBGPRINT("Stopping node\n");

    DPS_LockNode(node);
    StopNode(node);
    /*
     * If we got here before the application called DPS_DestroyNode() we cannot free the node now,
     * it will be freed when DPS_DestroyNode() is called.
     */
    if (node->onDestroyed) {
        DPS_UnlockNode(node);
        node->onDestroyed(node, node->onDestroyedData);
        uv_mutex_destroy(&node->nodeMutex);
        FreeNode(node);
        node = NULL;
    } else {
        DPS_UnlockNode(node);
    }

#ifdef DPS_DEBUG
    if (DPS_DEBUG_ENABLED()) {
        DPS_Log(DPS_LOG_DBGPRINT, __FILE__, __LINE__, __FUNCTION__,
                node ? node->addrStr : NULL, "Exiting node thread\n");
    }
#endif

    /*
     * Note: this is not currently a libuv API and is implemented locally
     */
    r = uv_thread_detach(&thisThread);
    if (r) {
        DPS_Log(DPS_LOG_ERROR, __FILE__, __LINE__, __FUNCTION__,
                node ? node->addrStr : NULL, "Failed to detatch thread: %s\n", uv_err_name(r));
    }
}

static DPS_Status SetCurve(DPS_KeyStoreRequest* request, const DPS_Key* key)
{
    DPS_ECCurve* curve = request->data;
    uint8_t d[EC_MAX_COORD_LEN];

    switch (key->type) {
    case DPS_KEY_EC:
        *curve = key->ec.curve;
        return DPS_OK;
    case DPS_KEY_EC_CERT:
        if (key->cert.privateKey) {
            return ParsePrivateKey_ECDSA(key->cert.privateKey, key->cert.password,
                    curve, d);
        }
        /* FALLTHROUGH */
    default:
        return DPS_ERR_MISSING;
    }
}

static DPS_Status GetSignatureAlgorithm(DPS_KeyStore* keyStore, const DPS_KeyId* keyId, int8_t* alg)
{
    DPS_KeyStoreRequest request;
    DPS_ECCurve curve = DPS_EC_CURVE_RESERVED;
    DPS_Status ret;

    if (!keyStore || !keyStore->keyHandler) {
        return DPS_ERR_MISSING;
    }

    memset(&request, 0, sizeof(request));
    request.keyStore = keyStore;
    request.data = &curve;
    request.setKey = SetCurve;
    ret = keyStore->keyHandler(&request, keyId);
    if (ret != DPS_OK) {
        return ret;
    }
    switch (curve) {
    case DPS_EC_CURVE_P384:
        *alg = COSE_ALG_ES384;
        break;
    case DPS_EC_CURVE_P521:
        *alg = COSE_ALG_ES512;
        break;
    default:
        ret = DPS_ERR_MISSING;
        break;
    }
    return ret;
}

DPS_Node* DPS_CreateNode(const char* separators, DPS_KeyStore* keyStore, const DPS_KeyId* keyId)
{
    DPS_Node* node = calloc(1, sizeof(DPS_Node));
    DPS_Status ret;

    DPS_DBGTRACE();

    if (!node) {
        return NULL;
    }
    /*
     * One time initilization required
     */
    if (DPS_InitUUID() != DPS_OK) {
        FreeNode(node);
        return NULL;
    }
    node->rbg = DPS_CreateRBG();
    if (!node->rbg) {
        FreeNode(node);
        return NULL;
    }
    if (!separators) {
        separators = "/";
    }
    /*
     * Sanity check
     */
    if (keyId && (!keyStore || !keyStore->keyHandler)) {
        DPS_WARNPRINT("A key request callback is required\n");
        FreeNode(node);
        return NULL;
    }
    if (keyId && (!keyId->id || !keyId->len)) {
        DPS_WARNPRINT("A valid key ID is required\n");
        FreeNode(node);
        return NULL;
    }
    if (keyId) {
        ret = GetSignatureAlgorithm(keyStore, keyId, &node->signer.alg);
        if (ret != DPS_OK) {
            DPS_WARNPRINT("Node ID not suitable for signing\n");
            ret = DPS_OK;
        }
        if (!DPS_CopyKeyId(&node->signer.kid, keyId)) {
            DPS_ERRPRINT("Allocate ID failed\n");
            FreeNode(node);
            return NULL;
        }
    }
    strncpy_s(node->separators, sizeof(node->separators), separators, sizeof(node->separators) - 1);
    node->keyStore = keyStore;
    DPS_QueueInit(&node->ackQueue);
    DPS_QueueInit(&node->requestQueue);
    /*
     * Set default keep alive and subscription rate parameters
     */
    node->subsRate = DPS_SUBSCRIPTION_UPDATE_RATE;
    node->linkLossTimeout = DPS_LINK_LOSS_TIMEOUT;
    return node;
}

DPS_Status DPS_SetNodeData(DPS_Node* node, void* data)
{
    if (node) {
        node->userData = data;
        return DPS_OK;
    } else {
        return DPS_ERR_NULL;
    }
}

void* DPS_GetNodeData(const DPS_Node* node)
{
    return node ? node->userData : NULL;
}

static void StopNodeTask(uv_async_t* handle)
{
    /*
     * Stopping the loop will cleanly stop the node
     */
    uv_stop(handle->loop);
}

static void RunRequestsTask(uv_async_t* handle)
{
    DPS_Node* node = handle->data;
    NodeRequest* request;

    DPS_LockNode(node);
    while (!DPS_QueueEmpty(&node->requestQueue)) {
        request = (NodeRequest*)DPS_QueueFront(&node->requestQueue);
        DPS_QueueRemove(&request->queue);
        DPS_UnlockNode(node);
        request->cb(request);
        DPS_LockNode(node);
    }
    DPS_UnlockNode(node);
}

DPS_Status DPS_StartNode(DPS_Node* node, int mcast, DPS_NodeAddress* listenAddr)
{
    DPS_Status ret = DPS_OK;
    int r;

    DPS_DBGTRACE();

    if (!node) {
        return DPS_ERR_NULL;
    }
    node->history.loop = node->loop = calloc(1, sizeof(uv_loop_t));
    if (!node->loop) {
        return DPS_ERR_RESOURCES;
    }
    r = uv_loop_init(node->loop);
    if (r) {
        free(node->loop);
        node->loop = NULL;
        node->history.loop = NULL;
        return DPS_ERR_FAILURE;
    }
    DPS_DBGPRINT("libuv version %s\n", uv_version_string());
    /*
     * Setup the asyncs for running background tasks
     */
    node->acksAsync.data = node;
    r = uv_async_init(node->loop, &node->acksAsync, SendAcksTask);
    assert(!r);

    node->pubsAsync.data = node;
    r = uv_async_init(node->loop, &node->pubsAsync, SendPubsTask);
    assert(!r);

    node->pubsTimer.data = node;
    r = uv_timer_init(node->loop, &node->pubsTimer);
    assert(!r);

    node->subsAsync.data = node;
    r = uv_async_init(node->loop, &node->subsAsync, SendSubsTask);
    assert(!r);

    node->freeAsync.data = node;
    r = uv_async_init(node->loop, &node->freeAsync, FreeTask);
    assert(!r);

    node->stopAsync.data = node;
    r = uv_async_init(node->loop, &node->stopAsync, StopNodeTask);
    assert(!r);

    node->resolverAsync.data = node;
    r = uv_async_init(node->loop, &node->resolverAsync, DPS_AsyncResolveAddress);
    assert(!r);

    node->subsTimer.data = node;
    r = uv_timer_init(node->loop, &node->subsTimer);
    assert(!r);
    uv_timer_start(&node->subsTimer, SendSubsTimer, node->linkLossTimeout, node->linkLossTimeout);

#ifndef _WIN32
    node->sigusr1.data = node;
    r = uv_signal_init(node->loop, &node->sigusr1);
    assert(!r);
    r = uv_signal_start(&node->sigusr1, DumpNode, SIGUSR1);
    assert(!r);
#endif

    node->requestAsync.data = node;
    r = uv_async_init(node->loop, &node->requestAsync, RunRequestsTask);
    assert(!r);

    /*
     * Mutex for protecting the node
     */
    r = uv_mutex_init(&node->condMutex);
    assert(!r);
    r = uv_mutex_init_recursive(&node->nodeMutex);
    assert(!r);
    r = uv_mutex_init(&node->history.lock);
    assert(!r);

    DPS_GenerateUUID(&node->meshId);
    DPS_DBGPRINT("Node mesh id is: %s\n", DPS_UUIDToString(&node->meshId));
    node->interests = DPS_CountVectorAlloc();
    node->needs = DPS_CountVectorAllocFH();
    node->scratch.interests = DPS_BitVectorAlloc();
    node->scratch.needs = DPS_BitVectorAllocFH();
    if (!node->interests || !node->needs || !node->scratch.interests || !node->scratch.needs) {
        ret = DPS_ERR_RESOURCES;
        goto ErrExit;
    }

    DPS_NodeRequestInit(node, &node->onShutdownReq, OnShutdownRequest);
    node->onShutdownReq.data = node;

    node->mcastPub = mcast;
    if (node->mcastPub & DPS_MCAST_PUB_ENABLE_SEND) {
        node->mcastSender = DPS_MulticastStartSend(node);
    }
    if (node->mcastPub & DPS_MCAST_PUB_ENABLE_RECV) {
        node->mcastReceiver = DPS_MulticastStartReceive(node, OnMulticastReceive);
    }
    node->netCtx = DPS_NetStart(node, listenAddr, OnNetReceive);
    if (!node->netCtx) {
        DPS_ERRPRINT("Failed to initialize network context on %s\n", DPS_NodeAddrToString(listenAddr));
        ret = DPS_ERR_NETWORK;
        goto ErrExit;
    }
    /*
     * Make sure have the listening address before we return
     */
    DPS_NetGetListenAddress(&node->addr, node->netCtx);
    strncpy_s(node->addrStr, sizeof(node->addrStr),
            DPS_NodeAddrToString(&node->addr), DPS_NODE_ADDRESS_MAX_STRING_LEN);
    /*
     *  The node loop gets its own thread to run on
     */
    r = uv_thread_create(&node->thread, NodeRun, node);
    if (r) {
        DPS_ERRPRINT("Failed to create node thread\n");
        ret = DPS_ERR_FAILURE;
        goto ErrExit;
    }
    node->state = DPS_NODE_RUNNING;
    return DPS_OK;

ErrExit:

    DPS_LockNode(node);
    StopNode(node);
    DPS_UnlockNode(node);
    return ret;

}

DPS_NetContext* DPS_GetNetContext(DPS_Node* node)
{
    return node->netCtx;
}

const DPS_NodeAddress* DPS_GetListenAddress(DPS_Node* node)
{
    return &node->addr;
}

const char* DPS_GetListenAddressString(DPS_Node* node)
{
    return node->addrStr;
}

DPS_Status DPS_DestroyNode(DPS_Node* node, DPS_OnNodeDestroyed cb, void* data)
{
    DPS_DBGTRACE();

    if (!node || !cb) {
        return DPS_ERR_NULL;
    }
    if (node->state == DPS_NODE_STOPPING) {
        return DPS_ERR_INVALID;
    }
    /*
     * Node might be destroyed before it was started
     */
    if (node->state != DPS_NODE_CREATED) {
        DPS_LockNode(node);
        if (node->state == DPS_NODE_RUNNING) {
            node->state = DPS_NODE_STOPPING;
            node->onDestroyed = cb;
            node->onDestroyedData = data;
            uv_async_send(&node->stopAsync);
            DPS_UnlockNode(node);
            return DPS_OK;
        }
        DPS_UnlockNode(node);
        assert(node->state == DPS_NODE_STOPPED);
        uv_mutex_destroy(&node->nodeMutex);
    }
    FreeNode(node);
    return DPS_ERR_NODE_DESTROYED;
}

void DPS_SetNodeSubscriptionUpdateDelay(DPS_Node* node, uint32_t subsRateMsecs)
{
    DPS_DBGTRACE();

    node->subsRate = subsRateMsecs;
}

void DPS_SetNodeLinkLossTimeout(DPS_Node* node, uint32_t linkLossMsecs)
{
    DPS_DBGTRACE();

    node->linkLossTimeout = linkLossMsecs;
}

static void LinkExists(NodeRequest* req)
{
    DPS_Status status = DPS_OK;
    OnOpCompletion* completion = (OnOpCompletion*)req->data;
    DPS_Node* node = req->node;

    DPS_LockNode(node);
    /*
     * If the local node didn't make the link request complete with an
     * OK status status, otherwise report an EXISTS error.
     */
    if (completion->remote->outbound.linkRequested) {
        status = DPS_ERR_EXISTS;
    } else {
        completion->remote->outbound.linkRequested = DPS_TRUE;
    }
    DPS_RemoteCompletion(completion, status);
    DPS_UnlockNode(node);
}

static DPS_Status Link(DPS_Node* node, const DPS_NodeAddress* addr, OnOpCompletion* completion)
{
    RemoteNode* remote = NULL;
    DPS_Status ret;

    DPS_DBGTRACEA("Link to %s\n", DPS_NodeAddrToString(addr));

    assert(addr);
    memcpy(&completion->addr, addr, sizeof(DPS_NodeAddress));
    DPS_LockNode(node);
    ret = DPS_AddRemoteNode(node, addr, NULL, &remote);
    if (ret == DPS_OK) {
        remote->outbound.linkRequested = DPS_TRUE;
        remote->state = REMOTE_LINKING;
        /*
         * Send the initial subscription to the remote node.
         */
        DPS_UpdateSubs(node, SubsSendNow);
    } else if (ret == DPS_ERR_EXISTS) {
        if (remote->completion) {
            /*
             * Operations must be serialized
             */
            ret = DPS_ERR_BUSY;
        } else {
            /*
             * Schedule a call to the completion callback.
             */
            ret = DPS_NodeRequestSchedule(&completion->req);
        }
    } else {
        DPS_ERRPRINT("Link failed - %s\n", DPS_ErrTxt(ret));
    }
    if (ret == DPS_OK) {
        assert(!remote->completion);
        completion->remote = remote;
        remote->completion = completion;
    }
    DPS_UnlockNode(node);
    return ret;
}

static void OnResolve(DPS_Node* node, const DPS_NodeAddress* addr, void* data)
{
    OnOpCompletion* completion = (OnOpCompletion*)data;
    DPS_Status ret;

    if (addr) {
        ret = Link(node, addr, completion);
    } else {
        ret = DPS_ERR_UNRESOLVED;
    }
    if (ret != DPS_OK) {
        DPS_LockNode(node);
        DPS_RemoteCompletion(completion, ret);
        DPS_UnlockNode(node);
    }
}

DPS_Status DPS_LinkRemoteAddr(DPS_Node* node, const DPS_NodeAddress* addr, DPS_OnLinkComplete cb, void* data)
{
    DPS_Status ret = DPS_OK;
    OnOpCompletion* completion = NULL;

    if (!addr || !node) {
        return DPS_ERR_NULL;
    }
    completion = AllocCompletion(node, NULL, LINK_OP, data, cb);
    if (completion) {
        ret = Link(node, addr, completion);
    } else {
        ret = DPS_ERR_RESOURCES;
    }
    if (ret != DPS_OK) {
        if (completion) {
            free(completion);
        }
    }
    return ret;
}

DPS_Status DPS_Link(DPS_Node* node, const char* addrText, DPS_OnLinkComplete cb, void* data)
{
    DPS_Status ret = DPS_OK;
    OnOpCompletion* completion = NULL;
    DPS_NodeAddress* addr = NULL;
#if defined(DPS_USE_DTLS) || defined(DPS_USE_TCP) || defined(DPS_USE_UDP)
    char host[DPS_MAX_HOST_LEN + 1];
    char service[DPS_MAX_SERVICE_LEN + 1];
#endif

    DPS_DBGTRACEA("node=%p,addrText=%s,cb=%p,data=%p\n", node, addrText, cb, data);

    if (!addrText || !node || !cb) {
        return DPS_ERR_NULL;
    }

    completion = AllocCompletion(node, NULL, LINK_OP, data, cb);
    if (!completion) {
        ret = DPS_ERR_RESOURCES;
        goto Exit;
    }
#if defined(DPS_USE_DTLS) || defined(DPS_USE_TCP) || defined(DPS_USE_UDP)
    ret = DPS_SplitAddress(addrText, host, sizeof(host), service, sizeof(service));
    if (ret != DPS_OK) {
        DPS_ERRPRINT("DPS_SplitAddress returned %s\n", DPS_ErrTxt(ret));
        goto Exit;
    }
    ret = DPS_ResolveAddress(node, host, service, OnResolve, completion);
    if (ret != DPS_OK) {
        DPS_ERRPRINT("DPS_ResolveAddress returned %s\n", DPS_ErrTxt(ret));
        goto Exit;
    }
#elif defined(DPS_USE_PIPE)
    addr = DPS_CreateAddress();
    if (!addr) {
        ret = DPS_ERR_RESOURCES;
        goto Exit;
    }
    if (DPS_SetAddress(addr, addrText) == NULL) {
        DPS_ERRPRINT("DPS_SetAddress failed\n");
        ret = DPS_ERR_INVALID;
        goto Exit;
    }
    ret = Link(node, addr, completion);
    if (ret != DPS_OK) {
        DPS_ERRPRINT("Link returned %s\n", DPS_ErrTxt(ret));
        goto Exit;
    }
#endif
Exit:
    DPS_DestroyAddress(addr);
    if (ret != DPS_OK) {
        if (completion) {
            free(completion);
        }
    }
    return ret;
}

static DPS_Status Unlink(DPS_Node* node, RemoteNode* remote, DPS_OnUnlinkComplete cb, void* data)
{
    assert(node);
    assert(remote);
#ifdef DPS_DEBUG
    assert(node->isLocked);
#endif
    /*
     * Operations must be serialized
     */
    if (remote->completion) {
        return DPS_ERR_BUSY;
    }
    /*
     * Clear so delete doesn't report a surprise link loss.
     */
    remote->outbound.linkRequested = DPS_FALSE;
    /*
     * Unlinking the remote node will cause it to be deleted after the
     * subscriptions are updated. When the remote node is removed
     * the completion callback will be called.
     */
    remote->completion = AllocCompletion(node, remote, UNLINK_OP, data, cb);
    if (!remote->completion) {
        return DPS_ERR_RESOURCES;
    }
    memcpy(&remote->completion->addr, &remote->ep.addr, sizeof(DPS_NodeAddress));
    /*
     * If there is a SAK pending we will set the state to UNLINKING
     * after the SAK has been received. See DPS_RemoteCompletion()
     */
    if (remote->outbound.sakPending) {
        DPS_DBGPRINT("Deferring unlink while SAK pending\n");
    } else {
        remote->state = REMOTE_UNLINKING;
        DPS_UpdateSubs(node, SubsSendNow);
    }
    return DPS_OK;
}

DPS_Status DPS_Unlink(DPS_Node* node, const DPS_NodeAddress* addr, DPS_OnUnlinkComplete cb, void* data)
{
    RemoteNode* remote;
    DPS_Status ret;

    DPS_DBGTRACE();

    if (!addr || !node || !cb) {
        return DPS_ERR_NULL;
    }
    DPS_LockNode(node);
    remote = DPS_LookupRemoteNode(node, addr);
    if (!remote) {
        DPS_UnlockNode(node);
        return DPS_ERR_MISSING;
    }
    ret = Unlink(node, remote, cb, data);
    if ((ret != DPS_OK) && (ret != DPS_ERR_BUSY)) {
        DPS_DeleteRemoteNode(node, remote);
    }
    DPS_UnlockNode(node);
    return ret;
}

DPS_Status DPS_ShutdownNode(DPS_Node* node, DPS_OnNodeShutdown cb, void* data)
{
    DPS_Status ret = DPS_OK;
    RemoteNode* remote;
    RemoteNode* next;

    DPS_DBGTRACEA("node=%p,cb=%p,data=%p\n", node, cb, data);

    if (!node || !cb) {
        return DPS_ERR_NULL;
    }
    if (node->state != DPS_NODE_RUNNING || node->onShutdown) {
        return DPS_ERR_INVALID;
    }
    DPS_LockNode(node);
    node->onShutdown = cb;
    node->onShutdownData = data;
    for (remote = node->remoteNodes; remote; remote = next) {
        next = remote->next;
        ret = Unlink(node, remote, OnShutdownUnlinkComplete, NULL);
        if (ret == DPS_ERR_BUSY) {
            /*
             * RemoteCompletion will finish the shutdown
             */
            ret = DPS_OK;
        }
        if (ret != DPS_OK) {
            DPS_ERRPRINT("Unlink failed - %s\n", DPS_ErrTxt(ret));
            DPS_DeleteRemoteNode(node, remote);
        }
    }
    if (!node->remoteNodes) {
        /*
         * Either no remote nodes exist or the Unlink calls above
         * failed, so schedule the callback.
         */
        ret = DPS_NodeRequestSchedule(&node->onShutdownReq);
    }
    DPS_UnlockNode(node);
    return ret;
}

const char* DPS_NodeAddrToString(const DPS_NodeAddress* addr)
{
    if (addr) {
        switch (addr->type) {
        case DPS_DTLS:
        case DPS_TCP:
        case DPS_UDP:
            return DPS_NetAddrText((const struct sockaddr*)&addr->u.inaddr);
        case DPS_PIPE:
            return addr->u.path;
        default:
            return "INVALID-ADDRESS";
        }
    }
    return "NULL";
}

DPS_NodeAddress* DPS_CreateAddress()
{
    return calloc(1, sizeof(DPS_NodeAddress));
}

void DPS_CopyAddress(DPS_NodeAddress* dest, const DPS_NodeAddress* src)
{
    if (dest && src) {
        *dest = *src;
    }
}

void DPS_DestroyAddress(DPS_NodeAddress* addr)
{
    if (addr) {
        free(addr);
    }
}

DPS_Status DPS_MakeNonce(const DPS_UUID* uuid, uint32_t seqNum, uint8_t msgType,
                         int8_t alg, DPS_RBG* rbg, uint8_t nonce[COSE_NONCE_LEN])
{
    uint8_t* p = nonce;

    switch (alg) {
    case COSE_ALG_RESERVED:
    case COSE_ALG_DIRECT:
        if (seqNum == 0) {
            /*
             * This means the seqNum has wrapped and we are at risk of
             * reusing the same nonce with a key
             */
            return DPS_ERR_NONCE_OVERFLOW;
        }
        *p++ = (uint8_t)(seqNum >> 0);
        *p++ = (uint8_t)(seqNum >> 8);
        *p++ = (uint8_t)(seqNum >> 16);
        *p++ = (uint8_t)(seqNum >> 24);
        memcpy_s(p, COSE_NONCE_LEN - sizeof(uint32_t), uuid, COSE_NONCE_LEN - sizeof(uint32_t));
        /*
         * Adjust one bit so nonce for PUB's and ACK's for same pub id
         * and sequence number are different
         */
        if (msgType == DPS_MSG_TYPE_PUB) {
            p[0] &= 0x7F;
        } else {
            p[0] |= 0x80;
        }
        return DPS_OK;
    case COSE_ALG_A256KW:
    case COSE_ALG_ECDH_ES_A256KW:
        return DPS_RandomBytes(rbg, nonce, COSE_NONCE_LEN);
    default:
        return DPS_ERR_NOT_IMPLEMENTED;
    }
}

DPS_KeyId* DPS_CopyKeyId(DPS_KeyId* dest, const DPS_KeyId* src)
{
    if (src->len) {
        dest->id = malloc(src->len);
        if (!dest->id) {
            return NULL;
        }
        dest->len = src->len;
        memcpy_s((uint8_t*)dest->id, dest->len, src->id, src->len);
    } else {
        dest->id = NULL;
        dest->len = 0;
    }
    return dest;
}

void DPS_ClearKeyId(DPS_KeyId* keyId)
{
    assert(keyId);
    if (keyId->id) {
        free((uint8_t*)keyId->id);
        keyId->id = NULL;
    }
}

void DPS_NodeRequestInit(DPS_Node* node, NodeRequest* req, OnNodeRequest cb)
{
    assert(req);
    DPS_QueueInit(&req->queue);
    req->node = node;
    req->cb = cb;
}

DPS_Status DPS_NodeRequestSchedule(NodeRequest* req)
{
    DPS_Node* node;
    DPS_Status ret = DPS_OK;
    int err;

    if (!req) {
        return DPS_ERR_ARGS;
    }
    node = req->node;

    DPS_LockNode(node);
    DPS_QueuePushBack(&node->requestQueue, &req->queue);
    err = uv_async_send(&node->requestAsync);
    if (err) {
        DPS_ERRPRINT("uv_async_send failed - %s\n", uv_strerror(err));
        ret = DPS_ERR_FAILURE;
        DPS_QueueRemove(&req->queue);
        free(req);
    }
    DPS_UnlockNode(node);
    return ret;
}

void DPS_NodeRequestCancel(NodeRequest* req)
{
    if (req) {
        DPS_LockNode(req->node);
        DPS_QueueRemove(&req->queue);
        DPS_UnlockNode(req->node);
    }
}

DPS_Status DPS_SetLinkLossCallback(DPS_Node* node, DPS_OnLinkLoss callback, void* data)
{
    if (!node) {
        return DPS_ERR_NULL;
    } else {
        DPS_LockNode(node);
        node->linkLossCB = callback;
        node->linkLossData = data;
        DPS_UnlockNode(node);
        return DPS_OK;
    }
}

#undef DPS_DBG_TAG
#define DPS_DBG_TAG NULL

static void DumpHistory(DPS_PubHistory* ph)
{
    DPS_NodeAddressList* addr;

    if (!ph) {
        return;
    }
    DumpHistory(ph->left);
    DPS_PRINT("  %s(%d)%s %"PRIu64" [", DPS_UUIDToString(&ph->id), ph->sn, ph->ackRequested ? " ACK" : "",
            ph->expiration);
    for (addr = ph->addrs; addr; addr = addr->next) {
        DPS_PRINT("%s%s(%d,%d)", addr != ph->addrs ? "," : "", DPS_NodeAddrToString(&addr->addr), addr->sn, addr->hopCount);
    }
    DPS_PRINT("]\n");
    DumpHistory(ph->right);
}

static void DumpNode(uv_signal_t* handle, int signum)
{
    DPS_Node* node = handle->data;
    DPS_Publication* pub;
    DPS_Subscription* sub;
    RemoteNode* remote;
    size_t i;

    DPS_LockNode(node);
    DPS_PRINT("node %s\n", DPS_NodeAddrToString(&node->addr));
    DPS_PRINT("publications\n");
    for (pub = node->publications; pub; pub = pub->next) {
        DPS_DumpPub(pub);
    }
    DPS_PRINT("subscriptions\n");
    for (sub = node->subscriptions; sub; sub = sub->next) {
        DPS_PRINT("  topics=[");
        for (i = 0; i < sub->numTopics; ++i) {
            DPS_PRINT("%s%s", i ? ",": "", sub->topics[i]);
        }
        DPS_PRINT("]\n");
    }
    DPS_PRINT("remoteNodes\n");
    for (remote = node->remoteNodes; remote; remote = remote->next) {
        DPS_PRINT("  %s state=%s,interests=%s\n", DPS_NodeAddrToString(&remote->ep.addr),
                  RemoteStateTxt(remote), DPS_DumpMatchingTopics(remote->inbound.interests));
    }
    DPS_PRINT("history\n");
    DumpHistory(node->history.root);
    DPS_UnlockNode(node);
}

const char* RemoteStateTxt(const RemoteNode* remote)
{
    if (remote) {
        switch (remote->state) {
        case REMOTE_NEW:
            return "NEW";
        case REMOTE_ACTIVE:
            return "ACTIVE";
        case REMOTE_LINKING:
            return "LINKING";
        case REMOTE_UNLINKING:
            return "UNLINKING";
        case REMOTE_MUTED:
            return "MUTED";
        case REMOTE_UNMUTING:
            return "UNMUTING";
        default:
            return "UNKNOWN";
        }
    } else {
        return "NULL";
    }
}
