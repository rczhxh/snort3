//--------------------------------------------------------------------------
// Copyright (C) 2014-2016 Cisco and/or its affiliates. All rights reserved.
// Copyright (C) 2005-2013 Sourcefire, Inc.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------

#include "expect_cache.h"

#include <assert.h>

#include "time/packet_time.h"
#include "stream/stream_api.h"  // FIXIT-M bad dependency
#include "hash/zhash.h"
#include "sfip/sf_ip.h"

/* Reasonably small, and prime */
// FIXIT-L size based on max_tcp + max_udp?
#define MAX_HASH 1021
#define MAX_LIST    8
#define MAX_DATA    4
#define MAX_WAIT  300
#define MAX_PRUNE   5

struct ExpectFlow
{
    struct ExpectFlow* next;
    FlowData* data;

    ~ExpectFlow();
    void clear();
};

ExpectFlow::~ExpectFlow()
{
    clear();
}

void ExpectFlow::clear()
{
    while ( data )
    {
        FlowData* fd = data;
        data = data->next;
        delete fd;
    }
    data = nullptr;
}

struct ExpectNode
{
    time_t expires = 0;
    int reversed_key = 0;
    int direction = 0;
    unsigned count = 0;
    int16_t appId = 0;

    ExpectFlow* head = nullptr;
    ExpectFlow* tail = nullptr;

    void clear(ExpectFlow*&);
};

void ExpectNode::clear(ExpectFlow*& list)
{
    while ( head )
    {
        ExpectFlow* p = head;
        head = head->next;
        p->clear();
        p->next = list;
        list = p;
    }
    tail = nullptr;
    count = 0;
}

struct ExpectKey
{
    sfip_t ip1;
    sfip_t ip2;
    uint16_t port1;
    uint16_t port2;
    PktType protocol;

    bool set(
        const sfip_t *cliIP, uint16_t cliPort,
        const sfip_t *srvIP, uint16_t srvPort,
        PktType proto);
};

inline bool ExpectKey::set(
    const sfip_t *cliIP, uint16_t cliPort,
    const sfip_t *srvIP, uint16_t srvPort,
    PktType proto )
{
    bool reverse;
    SFIP_RET rval = sfip_compare(cliIP, srvIP);

    if (rval == SFIP_LESSER || (rval == SFIP_EQUAL && cliPort < srvPort))
    {
        sfip_copy(ip1, cliIP);
        port1 = cliPort;
        sfip_copy(ip2, srvIP);
        port2 = srvPort;
        reverse = false;
    }
    else
    {
        sfip_copy(ip1, srvIP);
        port1 = srvPort;
        sfip_copy(ip2, cliIP);
        port2 = cliPort;
        reverse = true;
    }
    protocol = proto;
    return reverse;
}

//-------------------------------------------------------------------------
// private ExpectCache methods
//-------------------------------------------------------------------------

// Clean the hash table of at most MAX_PRUNE expired nodes
void ExpectCache::prune()
{
    time_t now = packet_time();

    for (unsigned i = 0; i < MAX_PRUNE; ++i )
    {
        ExpectNode* node = (ExpectNode*)hash_table->first();

        if ( !node || now <= node->expires )
            break;

        hash_table->remove();
        ++prunes;
    }
}

inline ExpectNode* ExpectCache::get_node(ExpectKey& key, bool& init)
{
    ExpectNode* node;

    if ( !free_list )
        node = nullptr;
    else
        node = (ExpectNode*)hash_table->get(&key);

    if ( node )
        init = false;

    else
    {
        prune();

        node = (ExpectNode*)hash_table->get(&key);

        if ( !node )
        {
            ++overflows;
            return nullptr;
        }
        else if ( !free_list )
        {
            assert(false);
            ++overflows;
            return nullptr;
        }
    }
    return node;
}

inline ExpectFlow* ExpectCache::get_flow(ExpectNode* node, unsigned flow_id, int16_t appId)
{
    if ( packet_time() > node->expires )
    {
        node->clear(free_list);
        node->appId = appId;
        ++prunes;
    }
    else if ( node->appId != appId )
    {
        if ( node->appId && appId )
            // reject changing known appId
            return nullptr;

        // allow changing unknown appId
        node->appId = appId;
    }
    ExpectFlow* last = node->tail;

    if ( !last )
        return nullptr;

    FlowData* fd = last->data;

    while ( fd )
    {
        if ( fd->get_id() == flow_id )
            return nullptr;

        fd = fd->next;
    }
    return last;
}

inline bool ExpectCache::set_data(ExpectNode* node, ExpectFlow*& last, FlowData* fd)
{
    if ( !last )
    {
        if ( node->count >= MAX_LIST )
        {
            // fail when maxed out
            ++overflows;
            return false;
        }
        last = free_list;
        free_list = free_list->next;

        if ( !node->tail )
            node->head = last;
        else
            node->tail->next = last;

        node->tail = last;
        last->next = nullptr;

        node->count++;
    }
    if (last->data)
    {
        FlowData* prev_fd;
        for (prev_fd = last->data; prev_fd && prev_fd->next; prev_fd = prev_fd->next);
        prev_fd->next = fd;
    }
    else
        last->data = fd;

    return true;
}

//-------------------------------------------------------------------------
// public ExpectCache methods
//-------------------------------------------------------------------------

ExpectCache::ExpectCache (uint32_t max)
{
    // -size forces use of abs(size) ie w/o bumping up
    hash_table = new ZHash(-MAX_HASH, sizeof(ExpectKey));

    nodes = new ExpectNode[max];

    for ( unsigned i = 0; i < max; ++i )
        hash_table->push(nodes+i);

    max *= MAX_LIST;

    pool = new ExpectFlow[max];
    free_list = nullptr;

    for ( unsigned i = 0; i < max; ++i )
    {
        ExpectFlow* p = pool + i;
        p->data = nullptr;
        p->next = free_list;
        free_list = p;
    }
    memset(&zeroed, 0, sizeof(zeroed));

    expects = realized = 0;
    prunes = overflows = 0;
}

ExpectCache::~ExpectCache ()
{
    delete hash_table;
    delete[] nodes;
    delete[] pool;
}

/**Either expect or expect future session.
 *
 * Preprocessors may add sessions to be expected altogether or to be associated
 * with some data. For example, FTP preprocessor may add data channel that
 * should be expected. Alternatively, FTP preprocessor may add session with
 * appId FTP-DATA.
 *
 * It is assumed that only one of cliPort or srvPort should be known (!0). This
 * violation of this assumption will cause hash collision that will cause some
 * session to be not expected and expected. This will occur only rarely and
 * therefore acceptable design optimization.
 *
 * Also, appId is assumed to be consistent between different preprocessors.
 * Each session can be assigned only one AppId. When new appId mismatches
 * existing appId, new appId and associated data is not stored.
 *
 */
int ExpectCache::add_flow(
    const sfip_t *cliIP, uint16_t cliPort,
    const sfip_t *srvIP, uint16_t srvPort,
    PktType protocol, char direction,
    FlowData* fd, int16_t appId)
{
    // FIXIT-L sip inspector knows both ports
    //assert(!cliPort || !srvPort);

    ExpectKey hashKey;
    int reversed_key = hashKey.set(cliIP, cliPort, srvIP, srvPort, protocol);

    bool init = true;
    ExpectNode* node = get_node(hashKey, init);

    if ( !node )
        return -1;

    ExpectFlow* last;

    if ( !init )
        last = get_flow(node, fd->get_id(), appId);

    else
    {
        node->appId = appId;
        node->reversed_key = reversed_key;
        node->direction = direction;
        node->head = node->tail = nullptr;
        node->count = 0;
        last = nullptr;
    }
    if ( !set_data(node, last, fd) )
        return -1;

    node->expires = packet_time() + MAX_WAIT;
    ++expects;

    return 0;
}

bool ExpectCache::is_expected(Packet* p)
{
    if ( !hash_table->get_count() )
        return false;

    const sfip_t* srcIP = p->ptrs.ip_api.get_src();
    const sfip_t* dstIP = p->ptrs.ip_api.get_dst();

    ExpectKey key;
    bool reversed_key = key.set(dstIP, p->ptrs.dp, srcIP, p->ptrs.sp, p->type());

    uint16_t port1;
    uint16_t port2;

    if ( reversed_key )
    {
        key.port2 = 0;
        port1 = 0;
        port2 = p->ptrs.sp;
    }
    else
    {
        key.port1 = 0;
        port1 = p->ptrs.sp;
        port2 = 0;
    }

    ExpectNode* node = (ExpectNode*)hash_table->find(&key);

    if ( !node )
    {
        // can't find with dp, so try sp ...
        key.port1 = port1;
        key.port2 = port2;

        node = (ExpectNode*)hash_table->find(&key);

        if ( !node )
            return false;
    }
    // FIXIT-M X This should also include a lookup in the table for entries where both
    // src and dst ports are known.
    if ( !node->head || (p->pkth->ts.tv_sec > node->expires) )
    {
        hash_table->remove();
        return false;
    }
    /* Make sure the packet direction is correct */
    switch (node->direction)
    {
    case SSN_DIR_BOTH:
        break;

    case SSN_DIR_FROM_CLIENT:
    case SSN_DIR_FROM_SERVER:
        if (node->reversed_key != reversed_key)
            return false;
        break;
    }

    return true;
}

char ExpectCache::process_expected(Packet* p, Flow* lws)
{
    int retVal = SSN_DIR_NONE;

    ExpectNode* node = (ExpectNode*)hash_table->current();

    if ( !node )
        return retVal;

    assert(node->count && node->head);

    node->count--;
    ExpectFlow* head = node->head;
    node->head = head->next;

    FlowData* fd;

    while ((fd = head->data))
    {
        head->data = fd->next;
        lws->set_application_data(fd);
        ++realized;
        fd->handle_expected(p);
    }
    head->next = free_list;
    free_list = head;

    /* If this is 0, we're ignoring, otherwise setting id of new session */
    if ( !node->appId )
        retVal = node->direction;

    else if ( lws->ssn_state.application_protocol != node->appId )
    {
        lws->ssn_state.application_protocol = node->appId;
    }

    if ( !node->count )
        hash_table->remove();

    return retVal;
}

char ExpectCache::check(Packet* p, Flow* lws)
{
    if ( !is_expected(p) )
        return SSN_DIR_NONE;

    return process_expected(p, lws);
}

