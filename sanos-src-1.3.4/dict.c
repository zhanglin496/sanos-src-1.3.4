/* Hash Tables Implementation.
 *
 * This file implements in memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto resize if needed
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
 *	 this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *	 notice, this list of conditions and the following disclaimer in the
 *	 documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *	 to endorse or promote products derived from this software without
 *	 specific prior written permission.
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


#ifndef __KERNEL__
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/time.h>
#include <ctype.h>
#include <assert.h>
#define zcalloc(x) calloc(1, x)
#define zmalloc(x) malloc(x)
#define zfree(x) free(x)
#else
#include <linux/string.h>
#include <linux/stddef.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/random.h>
#include <linux/ctype.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/rculist.h>
#define zcalloc(x) kzalloc(x, GFP_ATOMIC)
#define zmalloc(x) kmalloc(x, GFP_ATOMIC)
#define zfree(x) kfree(x)

#define random() random32()
#define assert(x)
#endif

#include "dict.h"


#define DICT_GC_MAX_BUCKETS_DIV	64u
#define DICT_GC_MAX_BUCKETS		8192u
#define DICT_GC_INTERVAL		(5 * HZ)
#define DICT_GC_MAX_EVICTS		256u

/* Using dictEnableResize() / dictDisableResize() we make possible to
 * enable/disable resizing of the hash table as needed. This is very important
 * for Redis, as we use copy-on-write and don't want to move too much memory
 * around when there is a child performing saving operations.
 *
 * Note that even when dict_can_resize is set to 0, not all resizes are
 * prevented: a hash table is still allowed to grow if the ratio between
 * the number of elements and the buckets > dict_force_resize_ratio. */

/* -------------------------- private prototypes ---------------------------- */

static int _dictinit(dict *ht, dicttype * type, void *privdataptr);
static dictentry *dictentry_find(dict *d, const void *key);

/* ----------------------------- API implementation ------------------------- */

static void dictentry_free_rcu(struct rcu_head *rcu)
{
	dictentry *entry = container_of(rcu, dictentry, d_rcu);
	zfree(entry);
}

void dictentry_free(dictentry *entry)
{
	dictfreekey(entry->d, entry);
	dictfreeval(entry->d, entry);
	call_rcu(&entry->d_rcu, dictentry_free_rcu);
}


void dictentry_delete(dictentry *entry)
{
	if (test_and_set_bit(DICTENTRY_DYING_BIT, &entry->flags))
		return;
	atomic_dec(&entry->d->ht[0].used);
	
	spin_lock_bh(&entry->d->lock);
	hlist_del_init_rcu(&entry->hnode);
	spin_unlock_bh(&entry->d->lock);

	dictentry_put(entry);
}

static void dict_gc_worker(struct work_struct *work)
{
	unsigned int i, goal, buckets = 0, expired_count = 0;
	unsigned long next_run = DICT_GC_INTERVAL;
	unsigned int ratio, scanned = 0;
	struct dictentry_gc_work *gc_work;
	struct hlist_head *hash;
	struct dict *d;
	unsigned int hashsz;

	gc_work = container_of(work, struct dictentry_gc_work, dwork.work);
	d = gc_work->d;
	i = gc_work->next_bucket;
	hash = d->ht[0].table;
	hashsz = d->ht[0].size;
	goal = min(hashsz / DICT_GC_MAX_BUCKETS_DIV, DICT_GC_MAX_BUCKETS);
	
	do {
		struct dictentry *entry;
		struct hlist_node *n;

		if (!atomic_read(&d->ht[0].used))
			break;
		rcu_read_lock();
		if (i >= hashsz)
			i = 0;

		hlist_for_each_entry_rcu(entry, n, &hash[i], hnode) {
			scanned++;
			if (dictentry_is_expired(entry) && !dictentry_is_dying(entry) &&
				atomic_inc_not_zero(&entry->ref)) {
				dictentry_delete(entry);
				dictentry_put(entry);
				expired_count++;
				continue;
			}
		}
		i++;
		
		rcu_read_unlock();
	} while (++buckets < goal &&
		 expired_count < DICT_GC_MAX_EVICTS);

	if (gc_work->exiting)
		return;

	/* if expired item more than 90% call worker now  */
	ratio = scanned ? expired_count * 100 / scanned : 0;
	if (ratio >= 90)
		next_run = 0;

	gc_work->next_bucket = i;
	schedule_delayed_work(&gc_work->dwork, next_run);
}

/* Reset a hash table already initialized with ht_init().
 * NOTE: This function should only be called by ht_destroy(). */
static void _dictreset(dictht *ht)
{
	ht->table = NULL;
	ht->size = 0;
	ht->sizemask = 0;
	atomic_set(&ht->used, 0);
}

/* Create a new hash table */
dict *dictcreate(dicttype *type, void *privdataptr)
{
	dict *d = zmalloc(sizeof(*d));
	if (!d)
		return NULL;
	
	if (_dictinit(d, type, privdataptr) != DICT_OK) {
		zfree(d);
		return NULL;
	}
	return d;
}

/* Initialize the hash table */
static int _dictinit(dict *d, dicttype *type, void *privdataptr)
{	
	int i;
	INIT_DELAYED_WORK(&d->dictentry_gc_work.dwork, dict_gc_worker);
	d->dictentry_gc_work.d = d;
	d->ht[0].size = type->dict_size ? type->dict_size : DICT_HT_INITIAL_SIZE;
	d->ht[0].sizemask = d->ht[0].size - 1;
	d->ht[0].table = zmalloc(d->ht[0].size * sizeof(struct hlist_head));
	
	if (!d->ht[0].table)
		return DICT_ERR;
	
	for (i = 0; i < d->ht[0].size; i++)
		INIT_HLIST_HEAD(&d->ht[0].table[i]);
	atomic_set(&d->ht[0].used, 0);
	d->type = type;
	d->privdata = privdataptr;
	spin_lock_init(&d->lock);
	if (type->gc)
		schedule_delayed_work(&d->dictentry_gc_work.dwork, DICT_GC_INTERVAL);
	
	return DICT_OK;
}

/* Add an element to the target hash table */
int dictadd(dict *d, void *key, void *val, unsigned long timeout, int init_ref)
{
	unsigned int idx;
	int ret;
	dictentry *entry;
	
	rcu_read_lock();
	entry = dictentry_find(d, key);
	rcu_read_unlock();
	
	if (entry)
		return DICT_ERR;
	
	spin_lock_bh(&d->lock);
	entry = dictentry_find_get(d, key);
	if (entry) {
		spin_unlock_bh(&d->lock);
		dictentry_put(entry);
		return DICT_ERR;
	}
	
	entry = zmalloc(sizeof(*entry));
	if (!entry)
		goto err_out;
	
	entry->flags = 0UL;
	entry->timeout = timeout + jiffies;
	entry->d = d;
	idx = dicthashkey(d, key) & d->ht[0].sizemask;
	ret = dictsetkey(d, entry, key, ret);
	if (ret != DICT_OK)
		goto err_out;
	
	ret = dictsetval(d, entry, val, ret);
	if (ret != DICT_OK) {
		dictfreekey(d, entry);
		goto err_out;
	}
	if (timeout == 0UL)
		set_bit(DICTENTRY_PERMANENT_BIT, &entry->flags);
	atomic_set(&entry->ref, init_ref);
	atomic_inc(&d->ht[0].used);
	hlist_add_head_rcu(&entry->hnode, &d->ht[0].table[idx]);
	spin_unlock_bh(&d->lock);

	return DICT_OK;
	
err_out:
	if (entry)
		zfree(entry);
	spin_unlock_bh(&d->lock);
	return DICT_ERR;
}

/* Destroy all hash entry */
static int _dictclear(dict *d, dictht *ht, void (callback) (void *))
{
	unsigned long i;
	struct hlist_head *hash;
	unsigned int hashsz;
	struct dictentry *entry;
	struct hlist_node *n, *pos;

	hash = d->ht[0].table;
	hashsz = d->ht[0].size;

	spin_lock_bh(&d->lock);
	for (i = 0; i < hashsz; i++) {
		hlist_for_each_entry_safe(entry, pos, n, &hash[i], hnode) {
			dictentry_get(entry);
			dictentry_delete(entry);
			dictentry_put(entry);
		}
	}
	spin_unlock_bh(&d->lock);

	return DICT_OK; 	/* never fails */
}

/* Clear & Release the hash table */
void dictrelease(dict *d)
{
	d->dictentry_gc_work.exiting = true;
	cancel_delayed_work_sync(&d->dictentry_gc_work.dwork);
	_dictclear(d, &d->ht[0], NULL);
	zfree(d->ht[0].table);
	/* Re-initialize the table */
	_dictreset(&d->ht[0]);
	rcu_barrier();
	zfree(d);
}

/* must be hold rcu_read_lock */
static dictentry *dictentry_find(dict *d, const void *key)
{
	dictentry *he;
	struct hlist_node *n;
	struct hlist_head *table;
	unsigned int h, idx;

	if (d->ht[0].size == 0)
		return NULL;		/* We don't have a table at all */
	
	table = d->ht[0].table;
	h = dicthashkey(d, key);
	idx = h & d->ht[0].sizemask;
	hlist_for_each_entry_rcu(he, n, &table[idx], hnode) {
		if (dictentry_is_expired(he)) {
			if (!dictentry_is_dying(he) && atomic_inc_not_zero(&he->ref)) {
				dictentry_delete(he);
				dictentry_put(he);
			}
			continue;
		}
		if (dictcomparekeys(d, key, he->key)) 
			return he;
	}
	
	return NULL;
}

dictentry *dictentry_find_get(dict *d, const void *key)
{
	dictentry *he;
	
	rcu_read_lock();
	he = dictentry_find(d, key);
	if (he && !atomic_inc_not_zero(&he->ref))
		he = NULL;
	rcu_read_unlock();
	return he;
}

void *dictentry_value(dictentry *he)
{
	return he ? dictgetval(he) : NULL;
}

void dictempty(dict *d, void (callback) (void *))
{
	_dictclear(d, &d->ht[0], callback);
	rcu_barrier();
}

void dictentry_find_and_kill(dict *d, const void *key)
{
	dictentry *he;

	he = dictentry_find_get(d, key);
	if (!he)
		return;
	dictentry_delete(he);
	dictentry_put(he);
}

/* caller must be have hold entry */
void dictentry_kill(dictentry *entry)
{
	dictentry_delete(entry);
	dictentry_put(entry);
}


#if 0

/* ----------------------- StringCopy Hash Table Type ------------------------*/

static unsigned int _dictStringCopyHTHashFunction(const void *key)
{
	return dictGenHashFunction(key, strlen(key));
}

static void *_dictStringDup(void *privdata, const void *key)
{
	int len = strlen(key);
	char *copy = zmalloc(len + 1);
	DICT_NOTUSED(privdata);

	memcpy(copy, key, len);
	copy[len] = '\0';
	return copy;
}

static int
_dictStringCopyHTKeyCompare(void *privdata, const void *key1,
				const void *key2)
{
	DICT_NOTUSED(privdata);

	return strcmp(key1, key2) == 0;
}

static void _dictStringDestructor(void *privdata, void *key)
{
	DICT_NOTUSED(privdata);

	zfree(key);
}

dicttype dictTypeHeapStringCopyKey = {
	_dictStringCopyHTHashFunction,	/* hash function */
	_dictStringDup,		/* key dup */
	NULL,			/* val dup */
	_dictStringCopyHTKeyCompare,	/* key compare */
	_dictStringDestructor,	/* key destructor */
	NULL			/* val destructor */
};

/* This is like StringCopy but does not auto-duplicate the key.
 * It's used for intepreter's shared strings. */
dicttype dictTypeHeapStrings = {
	_dictStringCopyHTHashFunction,	/* hash function */
	NULL,			/* key dup */
	NULL,			/* val dup */
	_dictStringCopyHTKeyCompare,	/* key compare */
	_dictStringDestructor,	/* key destructor */
	NULL			/* val destructor */
};

/* This is like StringCopy but also automatically handle dynamic
 * allocated C strings as values. */
dicttype dictTypeHeapStringCopyKeyValue = {
	_dictStringCopyHTHashFunction,	/* hash function */
	_dictStringDup,		/* key dup */
	_dictStringDup,		/* val dup */
	_dictStringCopyHTKeyCompare,	/* key compare */
	_dictStringDestructor,	/* key destructor */
	_dictStringDestructor,	/* val destructor */
};

int main(int argc, char **argv)
{
	int err;
	dict *d = dictcreate(&dictTypeHeapStringCopyKeyValue, NULL);
	err = dictadd(d, "1231", "456");
	printf("err=%d\n", err);
	err = dictadd(d, "1231", "456");
	printf("err=%d\n", err);
	printf("%s\n", dictentry_value(d, "1231"));
//	dictRelease(d);
//	dictPrintStats(d);
	dictrelease(d);
	return 0;
}

#endif
