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
#define zcalloc(x) kzalloc(x, GFP_ATOMIC)
#define zmalloc(x) kmalloc(x, GFP_ATOMIC)
#define zfree(x) kfree(x)

#define random() random32()
#define assert(x)
#endif

#include "dict.h"

/* Using dictEnableResize() / dictDisableResize() we make possible to
 * enable/disable resizing of the hash table as needed. This is very important
 * for Redis, as we use copy-on-write and don't want to move too much memory
 * around when there is a child performing saving operations.
 *
 * Note that even when dict_can_resize is set to 0, not all resizes are
 * prevented: a hash table is still allowed to grow if the ratio between
 * the number of elements and the buckets > dict_force_resize_ratio. */

//static int dict_can_resize = 1;
//static unsigned int dict_force_resize_ratio = 5;

/* -------------------------- private prototypes ---------------------------- */

static int _dictExpandIfNeeded(dict * ht);
static unsigned long _dictNextPower(unsigned long size);
static int _dictKeyIndex(dict * ht, const void *key);
static int _dictInit(dict * ht, dictType * type, void *privDataPtr);

/* ----------------------------- API implementation ------------------------- */

/* Reset a hash table already initialized with ht_init().
 * NOTE: This function should only be called by ht_destroy(). */
static void _dictreset(dictht * ht)
{
	ht->table = NULL;
	ht->size = 0;
	ht->sizemask = 0;
	ht->used = 0;
}

/* Create a new hash table */
dict *dictcreate(dictType * type, void *privDataPtr)
{
	dict *d = zmalloc(sizeof(*d));
	if (!d)
		return NULL;
	
	_dictinit(d, type, privDataPtr);
	return d;
}

/* Initialize the hash table */
static int _dictinit(dict * d, dictType * type, void *privdataptr)
{
	d->dictentry_gc_work.d = d;
	INIT_DELAYED_WORK(&d->dictentry_gc_work.dwork, dict_gc_worker);
	_dictreset(&d->ht[0]);
	d->type = type;
	d->privdata = privdataptr;
	d->rehashidx = -1;
	d->iterators = 0;
	return DICT_OK;
}

/* Add an element to the target hash table */
int dictadd(dict * d, void *key, void *val, unsigned long timeout)
{
	unsigned int idx;
	dictentry *entry;
	
	rcu_read_lock();
	entry = dictentry_find(d, key);
	rcu_read_unlock();
	
	if (entry)
			return DICT_ERR;
	
	spin_lock_bh(&d->lock);
	entry = dictentry_find_get(d, key);
	if (entry) {
		dictentry_put(entry);
		goto err_out;
	}
	
	entry = zmalloc(sizeof(*entry));
	if (!entry)
		goto err_out;
	
//	entry->d = d;
	entry->timeout = timeout + jiffies;
	idx = dicthashkey(d, key) & d->ht[0].sizemask;
	dictsetkey(d, entry, key);
	dictsetval(d, entry, val);
	
	atomic_set(&entry->ref, 1);
	hlist_add_head_rcu(&entry->hnode, &d->ht[0].table[idx]);
	spin_unlock_bh(&d->lock);

	return DICT_OK;
	
err_out:
	spin_unlock_bh(&d->lock);
	return DICT_ERR;
}

/* Low level add. This function adds the entry but instead of setting
 * a value returns the dictEntry structure to the user, that will make
 * sure to fill the value field as he wishes.
 *
 * This function is also directly exposed to the user API to be called
 * mainly in order to store non-pointers inside the hash value, example:
 *
 * entry = dictAddRaw(dict,mykey);
 * if (entry != NULL) dictSetSignedIntegerVal(entry,1000);
 *
 * Return values:
 *
 * If key already exists NULL is returned.
 * If key was added, the hash entry is returned to be manipulated by the caller.
 */
dictEntry *dictAddRaw(dict * d, void *key)
{
	int index;
	dictEntry *entry;
	dictht *ht;

	if (dictIsRehashing(d))
		_dictRehashStep(d);

	/* Get the index of the new element, or -1 if
	 * the element already exists. */
	if ((index = _dictKeyIndex(d, key)) == -1)
		return NULL;

	/* Allocate the memory and store the new entry */
	ht = dictIsRehashing(d) ? &d->ht[1] : &d->ht[0];
	entry = zmalloc(sizeof(*entry));
	if (!entry)
		return NULL;
	entry->next = ht->table[index];
	ht->table[index] = entry;
	ht->used++;

	/* Set the hash entry fields. */
	dictSetKey(d, entry, key);
	return entry;
}

/* Add an element, discarding the old if the key already exists.
 * Return 1 if the key was added from scratch, 0 if there was already an
 * element with such key and dictReplace() just performed a value update
 * operation. */
int dictReplace(dict * d, void *key, void *val)
{
	dictEntry *entry, auxentry;

	/* Try to add the element. If the key
	 * does not exists dictAdd will suceed. */
	if (dictAdd(d, key, val) == DICT_OK)
		return 1;
	/* It already exists, get the entry */
	entry = dictFind(d, key);
	/* Set the new value and free the old one. Note that it is important
	 * to do that in this order, as the value may just be exactly the same
	 * as the previous one. In this context, think to reference counting,
	 * you want to increment (set), and then decrement (free), and not the
	 * reverse. */
	auxentry = *entry;
	dictSetVal(d, entry, val);
	dictFreeVal(d, &auxentry);
	return 0;
}

/* dictReplaceRaw() is simply a version of dictAddRaw() that always
 * returns the hash entry of the specified key, even if the key already
 * exists and can't be added (in that case the entry of the already
 * existing key is returned.)
 *
 * See dictAddRaw() for more information. */
dictEntry *dictReplaceRaw(dict * d, void *key)
{
	dictEntry *entry = dictFind(d, key);

	return entry ? entry : dictAddRaw(d, key);
}

/* Search and remove an element */
static int dictGenericDelete(dict * d, const void *key, int nofree)
{
	unsigned int h, idx;
	dictEntry *he, *prevHe;
	int table;

	if (d->ht[0].size == 0)
		return DICT_ERR;	/* d->ht[0].table is NULL */
	if (dictIsRehashing(d))
		_dictRehashStep(d);
	h = dictHashKey(d, key);

	for (table = 0; table <= 1; table++) {
		idx = h & d->ht[table].sizemask;
		he = d->ht[table].table[idx];
		prevHe = NULL;
		while (he) {
			if (dictCompareKeys(d, key, he->key)) {
				/* Unlink the element from the list */
				if (prevHe)
					prevHe->next = he->next;
				else
					d->ht[table].table[idx] = he->next;
				if (!nofree) {
					dictFreeKey(d, he);
					dictFreeVal(d, he);
				}
				zfree(he);
				d->ht[table].used--;
				return DICT_OK;
			}
			prevHe = he;
			he = he->next;
		}
		if (!dictIsRehashing(d))
			break;
	}
	return DICT_ERR;		/* not found */
}

int dictDelete(dict * ht, const void *key)
{
	return dictGenericDelete(ht, key, 0);
}

int dictDeleteNoFree(dict * ht, const void *key)
{
	return dictGenericDelete(ht, key, 1);
}

/* Destroy an entire dictionary */
int _dictclear(dict * d, dictht * ht, void (callback) (void *))
{
	unsigned long i;

	/* Free all the elements */
	for (i = 0; i < ht->size && ht->used > 0; i++) {
		dictEntry *he, *nextHe;

		if (callback && (i & 65535) == 0)
			callback(d->privdata);

		if ((he = ht->table[i]) == NULL)
			continue;
		while (he) {
			nextHe = he->next;
			dictFreeKey(d, he);
			dictFreeVal(d, he);
			zfree(he);
			ht->used--;
			he = nextHe;
		}
	}
	/* Free the table and the allocated cache structure */
	zfree(ht->table);
	/* Re-initialize the table */
	_dictreset(ht);
	return DICT_OK;		/* never fails */
}

/* Clear & Release the hash table */
void dictrelease(dict * d)
{
	cancel_delayed_work_sync(&d->dictentry_gc_work.dwork);
	_dictclear(d, &d->ht[0], NULL);
	zfree(d);
}

/* must be hold rcu_read_lock */
static dictentry *dictentry_find(dict * d, const void *key)
{
	dictentry *he;
	struct hlist_node *n;
	struct hlist_head *table;
	unsigned int h, idx, table;

	if (d->ht[0].size == 0)
		return NULL;		/* We don't have a table at all */
	
	table = rcu_dereference(d->ht[0].table)
	h = dicthashkey(d, key);
	idx = h & d->ht[0].sizemask;
	hlist_for_each_entry_rcu(he, n, &table[idx], hnode) {
		if (dictcomparekeys(d, key, he->key)) {
			if (dictentry_is_expired(he)) {
				if (atomic_inc_not_zero(&he->ref))
					dictentry_kill(he);
				continue;
			}
			return he;
		}
	}
	
	return NULL;
}

dictentry *dictentry_find_get(dict * d, const void *key)
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

void dictempty(dict * d, void (callback) (void *))
{
	_dictclear(d, &d->ht[0], callback);
//	_dictclear(d, &d->ht[1], callback);
	d->rehashidx = -1;
	d->iterators = 0;
}

#if 1

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

dictType dictTypeHeapStringCopyKey = {
	_dictStringCopyHTHashFunction,	/* hash function */
	_dictStringDup,		/* key dup */
	NULL,			/* val dup */
	_dictStringCopyHTKeyCompare,	/* key compare */
	_dictStringDestructor,	/* key destructor */
	NULL			/* val destructor */
};

/* This is like StringCopy but does not auto-duplicate the key.
 * It's used for intepreter's shared strings. */
dictType dictTypeHeapStrings = {
	_dictStringCopyHTHashFunction,	/* hash function */
	NULL,			/* key dup */
	NULL,			/* val dup */
	_dictStringCopyHTKeyCompare,	/* key compare */
	_dictStringDestructor,	/* key destructor */
	NULL			/* val destructor */
};

/* This is like StringCopy but also automatically handle dynamic
 * allocated C strings as values. */
dictType dictTypeHeapStringCopyKeyValue = {
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
	dict *d = dictCreate(&dictTypeHeapStringCopyKeyValue, NULL);
	err = dictAdd(d, "1231", "456");
	printf("err=%d\n", err);
	err = dictAdd(d, "1231", "456");
	printf("err=%d\n", err);
	printf("%s\n", dictFetchValue(d, "1231"));
//	dictRelease(d);
	dictPrintStats(d);
	dictRelease(d);
	return 0;
}

#endif
