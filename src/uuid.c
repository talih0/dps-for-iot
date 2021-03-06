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

#ifdef _WIN32
#define _CRT_RAND_S
#endif

#include <safe_lib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include <dps/dbg.h>
#include <dps/uuid.h>
#include "compat.h"

/*
 * Debug control for this module
 */
DPS_DEBUG_CONTROL(DPS_DEBUG_OFF);

const char* DPS_UUIDToString(const DPS_UUID* uuid)
{
    static const char* hex = "0123456789abcdef";
    static THREAD char str[38];
    char* dst = str;
    const uint8_t *src = uuid->val + sizeof(uuid->val);
    size_t i;

    for (i = sizeof(uuid->val); i > 0; --i) {
        if (i == 6 || i == 8 || i == 10 || i == 12) {
            *dst++ = '-';
        }
        *dst++ = hex[*--src >> 4];
        *dst++ = hex[*src & 0xF];
    }
    *dst = 0;
    return str;
}

static struct {
    uint64_t nonce[2];
    uint32_t seeds[4];
} entropy;

static struct {
    uv_once_t once;
    DPS_Status ret;
    uv_mutex_t mutex;
} context = { UV_ONCE_INIT, DPS_OK, { 0 } };

#ifdef _WIN32
static void InitUUID(void)
{
    errno_t ret = 0;
    size_t i;
    uint32_t* n = (uint32_t*)&entropy;

    uv_mutex_init(&context.mutex);
    for (i = 0; i < (sizeof(entropy) / sizeof(uint32_t)); ++i) {
        ret = rand_s(n++);
        if (ret) {
            context.ret = DPS_ERR_FAILURE;
            break;
        }
    }
}
#else
/*
 * Linux specific implementation
 */
static const char* randPath = "/dev/urandom";

static void InitUUID(void)
{
    uv_mutex_init(&context.mutex);
    while (!entropy.nonce[0]) {
        size_t sz;
        FILE* f = fopen(randPath, "r");
        if (!f) {
            DPS_ERRPRINT("fopen(\"%s\", \"r\") failed\n", randPath);
            context.ret = DPS_ERR_READ;
            break;
        }
        sz = fread(&entropy, 1, sizeof(entropy), f);
        fclose(f);
        if (sz != sizeof(entropy)) {
            context.ret = DPS_ERR_READ;
            break;
        }
    }
}
#endif

DPS_Status DPS_InitUUID()
{
    DPS_DBGTRACE();

    uv_once(&context.once, InitUUID);
    return context.ret;
}

/*
 * Very simple linear congruational generator based PRNG (Lehmer/Park-Miller generator)
 */
#define LEPRNG(n)  (uint32_t)(((uint64_t)(n) * 279470273ull) % 4294967291ul)

/*
 * This is fast - not secure
 */
void DPS_GenerateUUID(DPS_UUID* uuid)
{
    uint64_t* s = (uint64_t*)entropy.seeds;
    uint32_t s0;

    DPS_DBGTRACE();

    uv_mutex_lock(&context.mutex);
    s0 = entropy.seeds[0];
    entropy.seeds[0] = LEPRNG(entropy.seeds[1]);
    entropy.seeds[1] = LEPRNG(entropy.seeds[2]);
    entropy.seeds[2] = LEPRNG(entropy.seeds[3]);
    entropy.seeds[3] = LEPRNG(s0);
    uuid->val64[0] = s[0] ^ entropy.nonce[0];
    uuid->val64[1] = s[1] ^ entropy.nonce[1];
    uv_mutex_unlock(&context.mutex);
}

int DPS_UUIDCompare(const DPS_UUID* a, const DPS_UUID* b)
{
    uint64_t al = a->val64[0];
    uint64_t ah = a->val64[1];
    uint64_t bl = b->val64[0];
    uint64_t bh = b->val64[1];
    return (ah < bh) ? -1 : ((ah > bh) ? 1 : ((al < bl) ? -1 : (al > bl) ? 1 : 0));
}

uint64_t DPS_Rand64(void)
{
    uint64_t s0;

    uv_mutex_lock(&context.mutex);
    s0 = entropy.seeds[0];
    entropy.seeds[0] = LEPRNG(entropy.seeds[1]);
    entropy.seeds[1] = LEPRNG(entropy.seeds[2]);
    entropy.seeds[2] = LEPRNG(entropy.seeds[3]);
    entropy.seeds[3] = LEPRNG(s0);
    s0 = entropy.seeds[1];
    s0 = (s0 << 32) | entropy.seeds[0];
    uv_mutex_unlock(&context.mutex);
    return s0;
}

/*
 * Note that uuidIn and uuidIn out may be aliased.
 */
void DPS_RandUUIDLess(const DPS_UUID* uuidIn, DPS_UUID* uuidOut)
{
    /*
     * Effectively this just subtracts a random 64 bit uint from a 128 bit uint
     */
    uint64_t l = uuidIn->val64[0] - DPS_Rand64();
    uint64_t h = uuidIn->val64[1];

    if (l >= uuidIn->val64[0]) {
        --h;
    }
    uuidOut->val64[0] = l;
    uuidOut->val64[1] = h;
}

uint32_t DPS_Rand(void)
{
    return (uint32_t)DPS_Rand64();
}
