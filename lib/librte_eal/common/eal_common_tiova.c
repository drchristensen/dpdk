/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2017-2018 Intel Corporation
 */

#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <sys/mman.h>
#include <stdint.h>
#include <errno.h>
#include <sys/file.h>
#include <sys/param.h>
#include <string.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_errno.h>
#include <rte_spinlock.h>
#include <rte_tailq.h>
#include "eal_internal_cfg.h"

#include "eal_common_tiova.h"

typedef struct iova_mem_desc iova_mem_desc_t;
struct iova_mem_desc {
  TAILQ_ENTRY(iova_mem_desc) next;
  uintptr_t vaddr;
  uintptr_t taddr;
  size_t len;
};

TAILQ_HEAD(iova_alloc_head, iova_mem_desc);

static struct iova_alloc_head iova_alloc_list =
	TAILQ_HEAD_INITIALIZER(iova_alloc_list);

TAILQ_HEAD(iova_free_head, iova_mem_desc);

static struct iova_free_head iova_free_list =
	TAILQ_HEAD_INITIALIZER(iova_free_list);

bool iova_overlap(iova_mem_desc_t *, const void *, const size_t);
void iova_sort(void);
void iova_compress(void);

/* DRC - Needed? */
//static rte_spinlock_t mem_area_lock = RTE_SPINLOCK_INITIALIZER;
// #define RTE_PTR_ADD(ptr, x) ((void*)((uintptr_t)(ptr) + (x)))

bool
iova_overlap(iova_mem_desc_t *id, const void *vaddr, const size_t len) {

	uintptr_t start = (uintptr_t)vaddr;
	uintptr_t end = start + len - 1;
	uintptr_t dsc_start = id->vaddr;
	uintptr_t dsc_end = id->vaddr + id->len - 1;

	// RTE_LOG(DEBUG, EAL, "DRC: %s(enter)...\n", __func__);

	/* if the two ranges share a single value, they overlap */
	if (RTE_MAX(start, dsc_start) <= RTE_MIN(end, dsc_end)) {
		RTE_LOG(DEBUG, EAL, "DRC: %s: comparing (V:0x%" PRIx64 "/L:0x%" PRIx64
			") to (V:0x%" PRIx64 "/L:0x%zx)\n",
			__func__, start, len, id->vaddr, id->len);
		RTE_LOG(DEBUG, EAL, "DRC: %s: found overlap!\n", __func__);
		return true;
	}

	// RTE_LOG(DEBUG, EAL, "DRC: %s: no overlap...\n", __func__);
	return false;
}


/*
 * Given a virtual memory window (start address + len), look for a matching
 * free window in the IOVA address space and add it to the allocated list
 * to support future virt2iova lookups.  No attempt is made to optimize
 * allocations, we use a first-fit algorithm.  Return the IOVA address for
 * the beginning of the window or RTE_BAD_IOVA if an error occurs.
 */
uint64_t
iova_alloc(const void *vaddr, const size_t len) {
	struct iova_mem_desc *f, *a, *t;

	RTE_LOG(DEBUG, EAL, "DRC: %s(enter): (V:0x%" PRIx64 "/"
		"L:0x%zx)\n", __func__, (uintptr_t) vaddr, len);

	/* nothing to allocate if the free list is empty */
	if (TAILQ_EMPTY(&iova_free_list)) {
		RTE_LOG(ERR, EAL, "%s: free list is empty...\n", __func__);
		return RTE_BAD_IOVA;
	}

	/* verify there's no overlap with an existing mapping */
	TAILQ_FOREACH(a, &iova_alloc_list, next) {
		if (iova_overlap(a, vaddr, len)) {
			RTE_LOG(ERR, EAL, "%s: found window overlap, "
				"aborting...\n", __func__);
			return RTE_BAD_IOVA;
		}
	}

	/* look for the first available mapping window in the free list */
	TAILQ_FOREACH_SAFE(f, &iova_free_list, next, t) {
		// RTE_LOG(DEBUG, EAL, "DRC: %s: checking free entry (T:0x%" PRIx64
		//	"/V:0x%" PRIx64 "/L:0x%zx)\n",
		//	__func__, f->taddr, f->vaddr, f->len);

		/* requested window too big? keep trying */
		if ((f->taddr + len) > (f->taddr + f->len))
			continue;

		RTE_LOG(DEBUG, EAL, "DRC: %s: found free space, allocating "
			"new IOVA window...\n", __func__);

		a = malloc(sizeof(*a));
		if (a == NULL) {
			RTE_LOG(ERR, EAL, "malloc failed during IOVA "
				"window allocation...\n");
			return RTE_BAD_IOVA;
		}

		/* allocate IOVA window from front of the free list */
		a->vaddr = (uintptr_t) vaddr;
		a->taddr = f->taddr;
		a->len   = len;
		TAILQ_INSERT_TAIL(&iova_alloc_list, a, next);
		RTE_LOG(DEBUG, EAL, "DRC: %s: new alloc entry (T:0x%" PRIx64
			"/V:0x%" PRIx64 "/L:0x%zx)\n",
			__func__, a->taddr, a->vaddr, a->len);

		/* adjust free list entry and remove allocated range */
		f->taddr = f->taddr + len;
		f->len   = f->len - len;
		if (f->len == 0) {
			/* remove the free list entry if it's empty */
			TAILQ_REMOVE(&iova_free_list, f, next);
			free(f);
			RTE_LOG(DEBUG, EAL, "DRC: %s: removed empty entry "
				"from free list...\n", __func__);
		} else {
			RTE_LOG(DEBUG, EAL, "DRC: %s: modified existing free "
				"entry (T:0x%" PRIx64 "/V:0x%" PRIx64
				"/L:0x%zx)\n", __func__, f->taddr,
				f->vaddr, f->len);
		}

		return a->taddr;
	} /* TAILQ_FOREACH_SAFE */

	RTE_LOG(ERR, EAL, "%s: insufficient free space for IOVA window...\n", __func__);
	return RTE_BAD_IOVA;
}

/*
 * We keep the iova_free_list sorted so adjacent free entries can be combined
 * to minimize fragmentation. The sort algorithm is run everytime the free list
 * is modified.
 */
void
iova_sort(void)
{
	/* current, previous, and temp pointers */
	struct iova_mem_desc *c = NULL, *p = NULL, *t;

	// RTE_LOG(DEBUG, EAL, "DRC: %s(enter)...\n", __func__);

	if (TAILQ_EMPTY(&iova_free_list)) {
		RTE_LOG(DEBUG, EAL, "DRC: %s: free list is empty, "
			"nothing to sort...\n", __func__);
		return;
	}

	TAILQ_FOREACH_SAFE(c, &iova_free_list, next, t) {
		// RTE_LOG(DEBUG, EAL, "DRC: %s: checking free (T:0x%" PRIx64
		//	"/V:0x%" PRIx64 "/L:0x%zx)\n",
		//	__func__, c->taddr, c->vaddr, c->len);

		/* need two entries to do anything useful */
		if (p == NULL) {
			p = c;
			continue;
		}

		/* swap the two adjacent entries */
		if (p->taddr > c->taddr) {
			RTE_LOG(DEBUG, EAL, "DRC: %s: swapping (T:0x%" PRIx64
				"/V:0x%" PRIx64 "/L:0x%zx)\n",
				__func__, p->taddr, p->vaddr, p->len);
			RTE_LOG(DEBUG, EAL, "DRC: %s:    and   (T:0x%" PRIx64
				"/V:0x%" PRIx64 "/L:0x%zx)\n",
				__func__, c->taddr, c->vaddr, c->len);
			TAILQ_REMOVE(&iova_free_list, p, next);
			TAILQ_INSERT_AFTER(&iova_free_list, c, p, next);
		}

		p = c;
	}

	// RTE_LOG(DEBUG, EAL, "DRC: %s: sort completed...\n", __func__);
}

/*
 * We keep entries in the iova_free_list as large as possible by combining
 * adjacent smaller entries into a single larger entry.
 */
void
iova_compress(void) {
	struct iova_mem_desc *c = NULL,*p = NULL, *t;

	// RTE_LOG(DEBUG, EAL, "DRC: %s(enter)...\n", __func__);

	/* Make sure the list is sorted */
	iova_sort();

	if (TAILQ_EMPTY(&iova_free_list)) {
		RTE_LOG(DEBUG, EAL, "DRC: %s: free list is empty, "
			"nothing to compress...\n", __func__);
		return;
	}

	TAILQ_FOREACH_SAFE(c, &iova_free_list, next, t) {

		/* need two entries to do anything useful */
		if (p == NULL) {
			p = c;
			continue;
		}

		/* entries are not adjacent, keep trying */
		if ((p->taddr + p->len) != c->taddr)
			continue;

		RTE_LOG(DEBUG, EAL, "DRC: %s: compressing (T:0x%" PRIx64
			"/V:0x%" PRIx64 "/L:0x%zx)\n", __func__,
			p->taddr, p->vaddr, p->len);
		RTE_LOG(DEBUG, EAL, "DRC: %s:     and     (T:0x%" PRIx64
			"/V:0x%" PRIx64 "/L:0x%zx)\n", __func__,
			c->taddr, c->vaddr, c->len);

		/* combine adjacent entries */
		p->len += c->len;
		TAILQ_REMOVE(&iova_free_list, c, next);
		free(c);
		RTE_LOG(DEBUG, EAL, "DRC: %s:   result    (T:0x%" PRIx64
			"/V:0x%" PRIx64 "/L:0x%zx)\n", __func__,
			p->taddr, p->vaddr, p->len);

	}
}

int
iova_free(const void *vaddr, const size_t len) {
	struct iova_mem_desc *a, *at;
	struct iova_mem_desc *f, *ft;
	bool free_match = false;
	bool alloc_match = false;

	RTE_LOG(DEBUG, EAL, "DRC: %s(enter): (V:0x%" PRIx64 "/L:0x%zx\n",
		__func__, (uintptr_t) vaddr, len);

	/* can't free an entry if the list is empty */
	if (TAILQ_EMPTY(&iova_alloc_list)) {
		RTE_LOG(ERR, EAL, "DRC: %s: attempted free from an empty "
			"alloc list...\n", __func__);
		return -1;
	}

	/* search the allocated list for a matching IOVA entry */
	TAILQ_FOREACH_SAFE(a, &iova_alloc_list, next, at) {
		// RTE_LOG(DEBUG, EAL, "DRC: %s: checking alloc (T:0x%" PRIx64
		//	"/V:0x%" PRIx64 "/L:0x%zx)\n",
		//	__func__, a->taddr, a->vaddr, a->len);

		/* check for a match, otherwise keep looking */
		if (((uintptr_t) vaddr != a->vaddr) || (len != a->len))
			continue;

		alloc_match = true;

		RTE_LOG(DEBUG, EAL, "DRC: %s: found a matching "
			"alloc entry, scanning free list...\n", __func__);

		/* can't combine with existing entry if the list is empty */
		/* DRC if (TAILQ_EMPTY(&iova_free_list)) */
		/* DRC	break; */

		/* search for an existing free entry that can be expanded */
		TAILQ_FOREACH_SAFE(f, &iova_free_list, next, ft) {
			RTE_LOG(DEBUG, EAL, "DRC: %s: checking free (T:0x%"
				PRIx64 "/V:0x%" PRIx64 "/L:0x%zx)\n",
				__func__, f->taddr, f->vaddr, f->len);

			if ((a->taddr + a->len) == f->taddr) {
				/* prepend to an existing free entry */
				RTE_LOG(DEBUG, EAL, "DRC: %s: prepending to "
					"an existing free list entry...\n", __func__);
				f->taddr = a->taddr;
				f->len   += a->len;
				free_match = true;
				break;
			} else if (a->taddr == (f->taddr + f->len)) {
				/* append to an existing free entry */
				RTE_LOG(DEBUG, EAL, "DRC: %s: appending to "
					"an existing free list entry...\n", __func__);
				f->len += a->len;
				free_match = true;
				break;
			}
		}

		/* can't expand an existing entry, create a new free entry */
		if (!free_match) {
			RTE_LOG(DEBUG, EAL, "%s: no suitable entries in the "
				"free list, adding a new entry...\n", __func__);
			f = malloc(sizeof(*f));
			if (f == NULL) {
				RTE_LOG(ERR, EAL, "malloc failed while mapping memory");
				return -1;
			}

			f->vaddr = 0;
			f->taddr = a->taddr;
			f->len   = a->len;
			TAILQ_INSERT_TAIL(&iova_free_list, f, next);
			RTE_LOG(DEBUG, EAL, "DRC: %s: created free (T:0x%"
				PRIx64 "/V:0x%" PRIx64 "/L:0x%zx)\n",
				__func__, f->taddr, f->vaddr, f->len);
		}

		/* keep the free list tidy */
		iova_compress();

		/* release the entry from the alloc list */
		TAILQ_REMOVE(&iova_alloc_list, a, next);
		free(a);
		break;
	} // TAILQ_FOREACH_SAFE()

	if (!alloc_match) {
		RTE_LOG(ERR, EAL, "DRC: %s: attempted free does not match "
			"any entries in the alloc list...\n", __func__);
		return -1;
	}

	return 0;
}

phys_addr_t
iova_search(const void *virtaddr)
{
	struct iova_mem_desc *a;
	uint64_t ret = RTE_BAD_IOVA;

	RTE_LOG(DEBUG, EAL, "DRC: %s(enter): (V:0x%" PRIx64 ")\n",
		__func__, (uintptr_t) virtaddr);

	if (TAILQ_EMPTY(&iova_alloc_list)) {
		RTE_LOG(DEBUG, EAL, "DRC: %s: iova_alloc is empty\n", __func__);
		goto out;
	}

	TAILQ_FOREACH(a, &iova_alloc_list, next) {
		// RTE_LOG(DEBUG, EAL, "DRC: %s: checking alloc entry (T:0x%" PRIx64
		//	"/V:0x%" PRIx64 "/L:0x%zx)\n", __func__,
		//	a->taddr, a->vaddr, a->len);
		if (((const uint64_t) virtaddr >= a->vaddr) &&
			((const uint64_t) virtaddr < a->vaddr + a->len)) {
			RTE_LOG(DEBUG, EAL, "DRC: %s: match (T:0x%" PRIx64
				"/V:0x%" PRIx64 ")\n", __func__,
				(a->taddr + ((const uint64_t) virtaddr - a->vaddr)),
				(uintptr_t) virtaddr);
			ret = a->taddr + ((const uint64_t) virtaddr - a->vaddr);
			goto out;
		}
	}
	RTE_LOG(DEBUG, EAL, "DRC: %s: no matching iova\n", __func__);

out:
	RTE_LOG(DEBUG, EAL, "DRC: %s(exit): (T:0x%" PRIx64 ")\n",
		__func__, ret);
	return ret;
}

int
iova_init(void) {
	struct iova_mem_desc *f;

	TAILQ_INIT(&iova_alloc_list);
	TAILQ_INIT(&iova_free_list);


	f = malloc(sizeof(*f));
	if (f == NULL) {
		RTE_LOG(ERR, EAL, "Error during malloc...\n");
		return -1;
	}

	// DRC f->taddr = (uintptr_t) taddr;
	f->taddr = internal_config.iova_base;
	f->vaddr = 0ULL;
	f->len   = internal_config.iova_len;
	RTE_LOG(DEBUG, EAL, "DRC: %s: initial free entry (T:0x%" PRIx64 "/V:0x%"
		PRIx64 "/L:0x%zx)\n", __func__, f->taddr, f->vaddr, f->len);
	TAILQ_INSERT_TAIL(&iova_free_list, f, next);

	return 0;
}



