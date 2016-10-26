/* Hash Tables Implementation.
 *
 * This file implements in-memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto-resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#ifndef __DICT_H
#define __DICT_H

#include <linux/list.h>

#define DICT_OK 0
#define DICT_ERR -1

enum dictentry_status {
	DICTENTRY_DYING_BIT,
	DICTENTRY_PERMANENT_BIT,
};

/* Unused arguments generate annoying warnings... */
#define DICT_NOTUSED(V) ((void) V)
struct dict;
typedef struct dictentry {
	void *key;
	union {
		void *val;
		uint64_t u64;
		int64_t s64;
	} v;
	atomic_t ref;
	unsigned long flags;
	unsigned long timeout;
	struct dict *d;
	struct rcu_head d_rcu;
	struct hlist_node hnode;
} dictentry;

typedef struct dicttype {
	u32 dict_limit;	/* hash item limit, 0 means no limit */
	u32 dict_size;	/* hashtable size, 0 means default DICT_HT_INITIAL_SIZE */
	bool gc;			/*need async garbage collect */
	unsigned int (*hashfunction)(const void *key);
	void *(*keydup)(void *privdata, const void *key, int *ret);
	void *(*valdup)(void *privdata, const void *obj, int *ret);
	int (*keycompare)(void *privdata, const void *key1, const void *key2);
	void (*keydestructor)(void *privdata, void *key);
	void (*valdestructor)(void *privdata, void *obj);
	int (*dict_upperlimit)(void *privdata, struct dict *d);
} dicttype;

/* This is our hash table structure. Every dictionary has two of this as we
 * implement incremental rehashing, for the old to the new table. */
typedef struct dictht {
	struct hlist_head *table;
	unsigned int size;
	unsigned int sizemask;
	atomic_t used;
} dictht;

struct dictentry_gc_work {
	struct delayed_work dwork;
	struct dict *d;
	u32	next_bucket;
	bool	exiting;
};

typedef struct dict {
	dicttype *type;
	void *privdata;
	spinlock_t lock;
	dictht ht[1];
	/* gc worker for cleanup timout entry */
	struct dictentry_gc_work dictentry_gc_work;
} dict;

/* This is the initial size of every hash table */
#define DICT_HT_INITIAL_SIZE	 128

/* ------------------------------- Macros ------------------------------------*/
#define dictfreeval(d, entry) \
	if ((d)->type->valdestructor) \
		(d)->type->valdestructor((d)->privdata, (entry)->v.val)

#define dictsetval(d, entry, _val_, _ret_) (({do { \
	if ((d)->type->valdup) \
		entry->v.val = (d)->type->valdup((d)->privdata, _val_, &(_ret_)); \
	else { \
		_ret_ = DICT_OK; \
		entry->v.val = (_val_); \
	} \
} while(0);}), _ret_)

#define dictsetsignedintegerval(entry, _val_) \
	do { entry->v.s64 = _val_; } while(0)

#define dictsetunsignedintegerval(entry, _val_) \
	do { entry->v.u64 = _val_; } while(0)

#define dictsetdoubleval(entry, _val_) \
	do { entry->v.d = _val_; } while(0)

#define dictfreekey(d, entry) \
	if ((d)->type->keydestructor) \
		(d)->type->keydestructor((d)->privdata, (entry)->key)

#define dictsetkey(d, entry, _key_, _ret_) (({do { \
	if ((d)->type->keydup) { \
		entry->key = (d)->type->keydup((d)->privdata, _key_, &(_ret_)); \
	}  else { \
		_ret_ = DICT_OK; \
		entry->key = (_key_);\
	} \
} while(0);}), _ret_)

#define dictcomparekeys(d, key1, key2) \
	(((d)->type->keycompare) ? \
		(d)->type->keycompare((d)->privdata, key1, key2) : \
		(key1) == (key2))

#define dicthashkey(d, key) (d)->type->hashfunction(key)
#define dictgetkey(he) ((he)->key)
#define dictgetval(he) ((he)->v.val)
#define dictgetsignedintegerval(he) ((he)->v.s64)
#define dictgetunsignedintegerval(he) ((he)->v.u64)
#define dictgetdoubleval(he) ((he)->v.d)
#define dictslots(d) ((d)->ht[0].size+(d)->ht[1].size)
#define dictsize(d) ((d)->ht[0].used+(d)->ht[1].used)
#define dictisrehashing(d) ((d)->rehashidx != -1)

/* API */
void dictentry_free(dictentry *entry);
dict *dictcreate(dicttype *type, void *privdataptr);
int dictadd(dict *d, void *key, void *val, unsigned long timeout, int init_ref);
void dictrelease(dict *d);
dictentry *dictentry_find_get(dict *d, const void *key);
void dictentry_find_and_kill(dict *d, const void *key);
void dictempty(dict *d, void(callback)(void*));

static inline void dictentry_put(dictentry *entry)
{
	if (entry && atomic_dec_and_test(&entry->ref))
		dictentry_free(entry);
}

static inline void dictentry_get(dictentry *entry)
{
	if (entry)
		atomic_inc(&entry->ref);
}

static inline int dictentry_is_expired(dictentry *dict)
{
	return !test_bit(DICTENTRY_PERMANENT_BIT, &dict->flags) && 
			time_after(jiffies, dict->timeout);
}

static inline int dictentry_is_dying(dictentry *dict)
{
	return test_bit(DICTENTRY_DYING_BIT, &dict->flags);
}

/* Hash table types */
//extern dictType dictTypeHeapStringCopyKey;
//extern dictType dictTypeHeapStrings;
//extern dictType dictTypeHeapStringCopyKeyValue;

#endif /* __DICT_H */
