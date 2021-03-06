/**
 * @file
 * Send and receive subscription messages
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

#ifndef _SUB_H
#define _SUB_H

#include <stdint.h>
#include <stddef.h>
#include <dps/private/dps.h>
#include "node.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Number of times the subscription timer can to trigger before
 * an unacknowledged subscription message (SUB or SAK) is resent.
 */
#define DPS_SAK_RETRY_THRESHOLD   2

/**
 * Number of times an unacknowledged subscription message (SUB or SAK)
 * is resent before the remote is considered to be unresponsive.
 */
#define DPS_SAK_RETRY_LIMIT       3

#define SUB_FLAG_WAS_FREED      (0x01) /**< The subscription has been freed but has a non-zero ref count */
#define SUB_FLAG_EXPIRED        (0x02) /**< Issue the callback function when a matching publication expires */
#define SUB_FLAG_SERIALIZE      (0x04) /**< Include the subscription in DPS_SerializeSubscriptions() */

/**
 * Struct to hold the state of a local subscription. We hold the
 * topics so we can provide return the topic list when we get a
 * match. We compute the filter so we can forward to outbound
 * subscribers.
 */
typedef struct _DPS_Subscription {
    void* userData;                 /**< Application provided user data */
    DPS_BitVector* needs;           /**< Subscription needs */
    DPS_BitVector* bf;              /**< The Bloom filter bit vector for the topics for this subscription */
    DPS_PublicationHandler handler; /**< Callback function to be called for a matching publication */
    DPS_Node* node;                 /**< Node for this subscription */
    uint32_t refCount;              /**< Ref count to prevent subscription from being freed while in use */
    uint8_t flags;                  /**< Internal state flags */
    DPS_OnSubscriptionDestroyed onDestroyed; /**< Optional on destroyed callback */
    DPS_Subscription* next; /**< Next subscription in list */
    size_t numTopics;       /**< Number of subscription topics */
    char* topics[1];        /**< Subscription topics */
} DPS_Subscription;

/**
 * Free a subscription
 *
 * @param sub The subscription
 *
 * @return The next subscription
 */
DPS_Subscription* DPS_FreeSubscription(DPS_Subscription* sub);

/**
 * Free all subscriptions registered with this node
 *
 * @param node  The node for this operation
 */
void DPS_FreeSubscriptions(DPS_Node* node);

/**
 * Increase a subscription's refcount to prevent it from being freed
 * from inside a callback function
 *
 * @param sub The subscription
 */
void DPS_SubscriptionIncRef(DPS_Subscription* sub);

/**
 * Decrease a subscription's refcount to allow it to be freed after
 * returning from a callback function
 *
 * @param sub The subscription
 */
void DPS_SubscriptionDecRef(DPS_Subscription* sub);

/**
 * Send a subscription (SUB) to a remote node
 *
 * @param node    The local node
 * @param remote  The remote node to send the SUB to
 *
 * @return DPS_OK if sending is successful, an error otherwise
 */
DPS_Status DPS_SendSubscription(DPS_Node* node, RemoteNode* remote);

/**
 * Send a subscription acknowledgement (SAK) to a remote node
 *
 * @param node      The local node
 * @param remote    The remote node to send the SAK to
 * @param collision SAK sent during collision with an incoming SUB
 *
 * @return DPS_OK if sending is successful, an error otherwise
 */
DPS_Status DPS_SendSubscriptionAck(DPS_Node* node, RemoteNode* remote, int collision);

/**
 * Decode and process a received subscription
 *
 * @param node       The local node
 * @param ep         The endpoint the subscription was received on
 * @param buffer     The encoded subscription
 *
 * @return DPS_OK if decoding and processing is successful, an error otherwise
 */
DPS_Status DPS_DecodeSubscription(DPS_Node* node, DPS_NetEndpoint* ep, DPS_NetRxBuffer* buffer);

/**
 * Decode and process a received subscription acknowledgement
 *
 * @param node       The local node
 * @param ep         The endpoint the subscription acknowledgement was received on
 * @param buffer     The encoded subscription acknowledgement
 *
 * @return DPS_OK if decoding and processing is successful, an error otherwise
 */
DPS_Status DPS_DecodeSubscriptionAck(DPS_Node* node, DPS_NetEndpoint* ep, DPS_NetRxBuffer* buffer);

#ifdef __cplusplus
}
#endif

#endif
