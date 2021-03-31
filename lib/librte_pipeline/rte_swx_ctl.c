/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2020 Intel Corporation
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/queue.h>
#include <unistd.h>

#include <rte_common.h>
#include <rte_byteorder.h>

#include "rte_swx_ctl.h"

#define CHECK(condition, err_code)                                             \
do {                                                                           \
	if (!(condition))                                                      \
		return -(err_code);                                            \
} while (0)

#define ntoh64(x) rte_be_to_cpu_64(x)
#define hton64(x) rte_cpu_to_be_64(x)

#if RTE_BYTE_ORDER == RTE_LITTLE_ENDIAN
#define field_ntoh(val, n_bits) (ntoh64((val) << (64 - n_bits)))
#define field_hton(val, n_bits) (hton64((val) << (64 - n_bits)))
#else
#define field_ntoh(val, n_bits) (val)
#define field_hton(val, n_bits) (val)
#endif

struct action {
	struct rte_swx_ctl_action_info info;
	struct rte_swx_ctl_action_arg_info *args;
	uint32_t data_size;
};

struct table {
	struct rte_swx_ctl_table_info info;
	struct rte_swx_ctl_table_match_field_info *mf;
	struct rte_swx_ctl_table_action_info *actions;
	struct rte_swx_table_ops ops;
	struct rte_swx_table_params params;

	/* Set of "stable" keys: these keys are currently part of the table;
	 * these keys will be preserved with no action data changes after the
	 * next commit.
	 */
	struct rte_swx_table_entry_list entries;

	/* Set of new keys: these keys are currently NOT part of the table;
	 * these keys will be added to the table on the next commit, if
	 * the commit operation is successful.
	 */
	struct rte_swx_table_entry_list pending_add;

	/* Set of keys to be modified: these keys are currently part of the
	 * table; these keys are still going to be part of the table after the
	 * next commit, but their action data will be modified if the commit
	 * operation is successful. The modify0 list contains the keys with the
	 * current action data, the modify1 list contains the keys with the
	 * modified action data.
	 */
	struct rte_swx_table_entry_list pending_modify0;
	struct rte_swx_table_entry_list pending_modify1;

	/* Set of keys to be deleted: these keys are currently part of the
	 * table; these keys are to be deleted from the table on the next
	 * commit, if the commit operation is successful.
	 */
	struct rte_swx_table_entry_list pending_delete;

	/* The pending default action: this is NOT the current default action;
	 * this will be the new default action after the next commit, if the
	 * next commit operation is successful.
	 */
	struct rte_swx_table_entry *pending_default;

	int is_stub;
	uint32_t n_add;
	uint32_t n_modify;
	uint32_t n_delete;
};

struct rte_swx_ctl_pipeline {
	struct rte_swx_ctl_pipeline_info info;
	struct rte_swx_pipeline *p;
	struct action *actions;
	struct table *tables;
	struct rte_swx_table_state *ts;
	struct rte_swx_table_state *ts_next;
	int numa_node;
};

static struct action *
action_find(struct rte_swx_ctl_pipeline *ctl, const char *action_name)
{
	uint32_t i;

	for (i = 0; i < ctl->info.n_actions; i++) {
		struct action *a = &ctl->actions[i];

		if (!strcmp(action_name, a->info.name))
			return a;
	}

	return NULL;
}

static void
action_free(struct rte_swx_ctl_pipeline *ctl)
{
	uint32_t i;

	if (!ctl->actions)
		return;

	for (i = 0; i < ctl->info.n_actions; i++) {
		struct action *action = &ctl->actions[i];

		free(action->args);
	}

	free(ctl->actions);
	ctl->actions = NULL;
}

static struct table *
table_find(struct rte_swx_ctl_pipeline *ctl, const char *table_name)
{
	uint32_t i;

	for (i = 0; i < ctl->info.n_tables; i++) {
		struct table *table = &ctl->tables[i];

		if (!strcmp(table_name, table->info.name))
			return table;
	}

	return NULL;
}

static int
table_params_get(struct rte_swx_ctl_pipeline *ctl, uint32_t table_id)
{
	struct table *table = &ctl->tables[table_id];
	uint8_t *key_mask = NULL;
	enum rte_swx_table_match_type match_type = RTE_SWX_TABLE_MATCH_WILDCARD;
	uint32_t key_size = 0, key_offset = 0, action_data_size = 0, i;

	if (table->info.n_match_fields) {
		struct rte_swx_ctl_table_match_field_info *first, *last;
		uint32_t i;

		first = &table->mf[0];
		last = &table->mf[table->info.n_match_fields - 1];

		/* match_type. */
		for (i = 0; i < table->info.n_match_fields; i++) {
			struct rte_swx_ctl_table_match_field_info *f;

			f = &table->mf[i];
			if (f->match_type != RTE_SWX_TABLE_MATCH_EXACT)
				break;
		}

		if (i == table->info.n_match_fields)
			match_type = RTE_SWX_TABLE_MATCH_EXACT;
		else if ((i == table->info.n_match_fields - 1) &&
			 (last->match_type == RTE_SWX_TABLE_MATCH_LPM))
			match_type = RTE_SWX_TABLE_MATCH_LPM;

		/* key_offset. */
		key_offset = first->offset / 8;

		/* key_size. */
		key_size = (last->offset + last->n_bits - first->offset) / 8;

		/* key_mask. */
		key_mask = calloc(1, key_size);
		CHECK(key_mask, ENOMEM);

		for (i = 0; i < table->info.n_match_fields; i++) {
			struct rte_swx_ctl_table_match_field_info *f;
			uint32_t start;
			size_t size;

			f = &table->mf[i];
			start = (f->offset - first->offset) / 8;
			size = f->n_bits / 8;

			memset(&key_mask[start], 0xFF, size);
		}
	}

	/* action_data_size. */
	for (i = 0; i < table->info.n_actions; i++) {
		uint32_t action_id = table->actions[i].action_id;
		struct action *a = &ctl->actions[action_id];

		if (a->data_size > action_data_size)
			action_data_size = a->data_size;
	}

	/* Fill in. */
	table->params.match_type = match_type;
	table->params.key_size = key_size;
	table->params.key_offset = key_offset;
	table->params.key_mask0 = key_mask;
	table->params.action_data_size = action_data_size;
	table->params.n_keys_max = table->info.size;

	return 0;
}

static void
table_entry_free(struct rte_swx_table_entry *entry)
{
	if (!entry)
		return;

	free(entry->key);
	free(entry->key_mask);
	free(entry->action_data);
	free(entry);
}

static struct rte_swx_table_entry *
table_entry_alloc(struct table *table)
{
	struct rte_swx_table_entry *entry;

	entry = calloc(1, sizeof(struct rte_swx_table_entry));
	if (!entry)
		goto error;

	/* key, key_mask. */
	if (!table->is_stub) {
		entry->key = calloc(1, table->params.key_size);
		if (!entry->key)
			goto error;

		if (table->params.match_type != RTE_SWX_TABLE_MATCH_EXACT) {
			entry->key_mask = calloc(1, table->params.key_size);
			if (!entry->key_mask)
				goto error;
		}
	}

	/* action_data. */
	if (table->params.action_data_size) {
		entry->action_data = calloc(1, table->params.action_data_size);
		if (!entry->action_data)
			goto error;
	}

	return entry;

error:
	table_entry_free(entry);
	return NULL;
}

static int
table_entry_key_check_em(struct table *table, struct rte_swx_table_entry *entry)
{
	uint8_t *key_mask0 = table->params.key_mask0;
	uint32_t key_size = table->params.key_size, i;

	if (!entry->key_mask)
		return 0;

	for (i = 0; i < key_size; i++) {
		uint8_t km0 = key_mask0[i];
		uint8_t km = entry->key_mask[i];

		if ((km & km0) != km0)
			return -EINVAL;
	}

	return 0;
}

static int
table_entry_check(struct rte_swx_ctl_pipeline *ctl,
		  uint32_t table_id,
		  struct rte_swx_table_entry *entry,
		  int key_check,
		  int data_check)
{
	struct table *table = &ctl->tables[table_id];
	int status;

	CHECK(entry, EINVAL);

	if (key_check) {
		if (table->is_stub) {
			/* key. */
			CHECK(!entry->key, EINVAL);

			/* key_mask. */
			CHECK(!entry->key_mask, EINVAL);
		} else {
			/* key. */
			CHECK(entry->key, EINVAL);

			/* key_mask. */
			switch (table->params.match_type) {
			case RTE_SWX_TABLE_MATCH_WILDCARD:
				break;

			case RTE_SWX_TABLE_MATCH_LPM:
				/* TBD Check that key mask is prefix. */
				break;

			case RTE_SWX_TABLE_MATCH_EXACT:
				status = table_entry_key_check_em(table, entry);
				if (status)
					return status;
				break;

			default:
				CHECK(0, EINVAL);
			}
		}
	}

	if (data_check) {
		struct action *a;
		uint32_t i;

		/* action_id. */
		for (i = 0; i < table->info.n_actions; i++)
			if (entry->action_id == table->actions[i].action_id)
				break;

		CHECK(i < table->info.n_actions, EINVAL);

		/* action_data. */
		a = &ctl->actions[entry->action_id];
		CHECK((a->data_size && entry->action_data) ||
		      (!a->data_size && !entry->action_data), EINVAL);
	}

	return 0;
}

static struct rte_swx_table_entry *
table_entry_duplicate(struct rte_swx_ctl_pipeline *ctl,
		      uint32_t table_id,
		      struct rte_swx_table_entry *entry,
		      int key_duplicate,
		      int data_duplicate)
{
	struct table *table = &ctl->tables[table_id];
	struct rte_swx_table_entry *new_entry = NULL;

	if (!entry)
		goto error;

	new_entry = calloc(1, sizeof(struct rte_swx_table_entry));
	if (!new_entry)
		goto error;

	if (key_duplicate && !table->is_stub) {
		/* key. */
		if (!entry->key)
			goto error;

		new_entry->key = malloc(table->params.key_size);
		if (!new_entry->key)
			goto error;

		memcpy(new_entry->key, entry->key, table->params.key_size);

		/* key_signature. */
		new_entry->key_signature = entry->key_signature;

		/* key_mask. */
		if (entry->key_mask) {
			new_entry->key_mask = malloc(table->params.key_size);
			if (!new_entry->key_mask)
				goto error;

			memcpy(new_entry->key_mask,
			       entry->key_mask,
			       table->params.key_size);
		}

		/* key_priority. */
		new_entry->key_priority = entry->key_priority;
	}

	if (data_duplicate) {
		struct action *a;
		uint32_t i;

		/* action_id. */
		for (i = 0; i < table->info.n_actions; i++)
			if (entry->action_id == table->actions[i].action_id)
				break;

		if (i >= table->info.n_actions)
			goto error;

		new_entry->action_id = entry->action_id;

		/* action_data. */
		a = &ctl->actions[entry->action_id];
		if (a->data_size && !entry->action_data)
			goto error;

		/* The table layer provisions a constant action data size per
		 * entry, which should be the largest data size for all the
		 * actions enabled for the current table, and attempts to copy
		 * this many bytes each time a table entry is added, even if the
		 * specific action requires less data or even no data at all,
		 * hence we always have to allocate the max.
		 */
		new_entry->action_data = calloc(1, table->params.action_data_size);
		if (!new_entry->action_data)
			goto error;

		if (a->data_size)
			memcpy(new_entry->action_data,
			       entry->action_data,
			       a->data_size);
	}

	return new_entry;

error:
	table_entry_free(new_entry);
	return NULL;
}

static int
table_entry_keycmp(struct table *table,
		   struct rte_swx_table_entry *e0,
		   struct rte_swx_table_entry *e1)
{
	uint32_t key_size = table->params.key_size;
	uint32_t i;

	for (i = 0; i < key_size; i++) {
		uint8_t *key_mask0 = table->params.key_mask0;
		uint8_t km0, km[2], k[2];

		km0 = key_mask0 ? key_mask0[i] : 0xFF;

		km[0] = e0->key_mask ? e0->key_mask[i] : 0xFF;
		km[1] = e1->key_mask ? e1->key_mask[i] : 0xFF;

		k[0] = e0->key[i];
		k[1] = e1->key[i];

		/* Mask comparison. */
		if ((km[0] & km0) != (km[1] & km0))
			return 1; /* Not equal. */

		/* Value comparison. */
		if ((k[0] & km[0] & km0) != (k[1] & km[1] & km0))
			return 1; /* Not equal. */
	}

	return 0; /* Equal. */
}

static struct rte_swx_table_entry *
table_entries_find(struct table *table, struct rte_swx_table_entry *entry)
{
	struct rte_swx_table_entry *e;

	TAILQ_FOREACH(e, &table->entries, node)
		if (!table_entry_keycmp(table, entry, e))
			return e; /* Found. */

	return NULL; /* Not found. */
}

static void
table_entries_free(struct table *table)
{
	for ( ; ; ) {
		struct rte_swx_table_entry *entry;

		entry = TAILQ_FIRST(&table->entries);
		if (!entry)
			break;

		TAILQ_REMOVE(&table->entries, entry, node);
		table_entry_free(entry);
	}
}

static struct rte_swx_table_entry *
table_pending_add_find(struct table *table, struct rte_swx_table_entry *entry)
{
	struct rte_swx_table_entry *e;

	TAILQ_FOREACH(e, &table->pending_add, node)
		if (!table_entry_keycmp(table, entry, e))
			return e; /* Found. */

	return NULL; /* Not found. */
}

static void
table_pending_add_admit(struct table *table)
{
	TAILQ_CONCAT(&table->entries, &table->pending_add, node);
}

static void
table_pending_add_free(struct table *table)
{
	for ( ; ; ) {
		struct rte_swx_table_entry *entry;

		entry = TAILQ_FIRST(&table->pending_add);
		if (!entry)
			break;

		TAILQ_REMOVE(&table->pending_add, entry, node);
		table_entry_free(entry);
	}
}

static struct rte_swx_table_entry *
table_pending_modify0_find(struct table *table,
			   struct rte_swx_table_entry *entry)
{
	struct rte_swx_table_entry *e;

	TAILQ_FOREACH(e, &table->pending_modify0, node)
		if (!table_entry_keycmp(table, entry, e))
			return e; /* Found. */

	return NULL; /* Not found. */
}

static void
table_pending_modify0_admit(struct table *table)
{
	TAILQ_CONCAT(&table->entries, &table->pending_modify0, node);
}

static void
table_pending_modify0_free(struct table *table)
{
	for ( ; ; ) {
		struct rte_swx_table_entry *entry;

		entry = TAILQ_FIRST(&table->pending_modify0);
		if (!entry)
			break;

		TAILQ_REMOVE(&table->pending_modify0, entry, node);
		table_entry_free(entry);
	}
}

static struct rte_swx_table_entry *
table_pending_modify1_find(struct table *table,
			   struct rte_swx_table_entry *entry)
{
	struct rte_swx_table_entry *e;

	TAILQ_FOREACH(e, &table->pending_modify1, node)
		if (!table_entry_keycmp(table, entry, e))
			return e; /* Found. */

	return NULL; /* Not found. */
}

static void
table_pending_modify1_admit(struct table *table)
{
	TAILQ_CONCAT(&table->entries, &table->pending_modify1, node);
}

static void
table_pending_modify1_free(struct table *table)
{
	for ( ; ; ) {
		struct rte_swx_table_entry *entry;

		entry = TAILQ_FIRST(&table->pending_modify1);
		if (!entry)
			break;

		TAILQ_REMOVE(&table->pending_modify1, entry, node);
		table_entry_free(entry);
	}
}

static struct rte_swx_table_entry *
table_pending_delete_find(struct table *table,
			  struct rte_swx_table_entry *entry)
{
	struct rte_swx_table_entry *e;

	TAILQ_FOREACH(e, &table->pending_delete, node)
		if (!table_entry_keycmp(table, entry, e))
			return e; /* Found. */

	return NULL; /* Not found. */
}

static void
table_pending_delete_admit(struct table *table)
{
	TAILQ_CONCAT(&table->entries, &table->pending_delete, node);
}

static void
table_pending_delete_free(struct table *table)
{
	for ( ; ; ) {
		struct rte_swx_table_entry *entry;

		entry = TAILQ_FIRST(&table->pending_delete);
		if (!entry)
			break;

		TAILQ_REMOVE(&table->pending_delete, entry, node);
		table_entry_free(entry);
	}
}

static void
table_pending_default_free(struct table *table)
{
	if (!table->pending_default)
		return;

	free(table->pending_default->action_data);
	free(table->pending_default);
	table->pending_default = NULL;
}

static int
table_is_update_pending(struct table *table, int consider_pending_default)
{
	struct rte_swx_table_entry *e;
	uint32_t n = 0;

	/* Pending add. */
	TAILQ_FOREACH(e, &table->pending_add, node)
		n++;

	/* Pending modify. */
	TAILQ_FOREACH(e, &table->pending_modify1, node)
		n++;

	/* Pending delete. */
	TAILQ_FOREACH(e, &table->pending_delete, node)
		n++;

	/* Pending default. */
	if (consider_pending_default && table->pending_default)
		n++;

	return n;
}

static void
table_free(struct rte_swx_ctl_pipeline *ctl)
{
	uint32_t i;

	if (!ctl->tables)
		return;

	for (i = 0; i < ctl->info.n_tables; i++) {
		struct table *table = &ctl->tables[i];

		free(table->mf);
		free(table->actions);
		free(table->params.key_mask0);

		table_entries_free(table);
		table_pending_add_free(table);
		table_pending_modify0_free(table);
		table_pending_modify1_free(table);
		table_pending_delete_free(table);
		table_pending_default_free(table);
	}

	free(ctl->tables);
	ctl->tables = NULL;
}

static void
table_state_free(struct rte_swx_ctl_pipeline *ctl)
{
	uint32_t i;

	if (!ctl->ts_next)
		return;

	/* For each table, free its table state. */
	for (i = 0; i < ctl->info.n_tables; i++) {
		struct table *table = &ctl->tables[i];
		struct rte_swx_table_state *ts = &ctl->ts_next[i];

		/* Default action data. */
		free(ts->default_action_data);

		/* Table object. */
		if (!table->is_stub && table->ops.free && ts->obj)
			table->ops.free(ts->obj);
	}

	free(ctl->ts_next);
	ctl->ts_next = NULL;
}

static int
table_state_create(struct rte_swx_ctl_pipeline *ctl)
{
	int status = 0;
	uint32_t i;

	ctl->ts_next = calloc(ctl->info.n_tables,
			      sizeof(struct rte_swx_table_state));
	if (!ctl->ts_next) {
		status = -ENOMEM;
		goto error;
	}

	for (i = 0; i < ctl->info.n_tables; i++) {
		struct table *table = &ctl->tables[i];
		struct rte_swx_table_state *ts = &ctl->ts[i];
		struct rte_swx_table_state *ts_next = &ctl->ts_next[i];

		/* Table object. */
		if (!table->is_stub && table->ops.add) {
			ts_next->obj = table->ops.create(&table->params,
							 &table->entries,
							 table->info.args,
							 ctl->numa_node);
			if (!ts_next->obj) {
				status = -ENODEV;
				goto error;
			}
		}

		if (!table->is_stub && !table->ops.add)
			ts_next->obj = ts->obj;

		/* Default action data: duplicate from current table state. */
		ts_next->default_action_data =
			malloc(table->params.action_data_size);
		if (!ts_next->default_action_data) {
			status = -ENOMEM;
			goto error;
		}

		memcpy(ts_next->default_action_data,
		       ts->default_action_data,
		       table->params.action_data_size);

		ts_next->default_action_id = ts->default_action_id;
	}

	return 0;

error:
	table_state_free(ctl);
	return status;
}

void
rte_swx_ctl_pipeline_free(struct rte_swx_ctl_pipeline *ctl)
{
	if (!ctl)
		return;

	action_free(ctl);

	table_state_free(ctl);

	table_free(ctl);

	free(ctl);
}

struct rte_swx_ctl_pipeline *
rte_swx_ctl_pipeline_create(struct rte_swx_pipeline *p)
{
	struct rte_swx_ctl_pipeline *ctl = NULL;
	uint32_t i;
	int status;

	if (!p)
		goto error;

	ctl = calloc(1, sizeof(struct rte_swx_ctl_pipeline));
	if (!ctl)
		goto error;

	/* info. */
	status = rte_swx_ctl_pipeline_info_get(p, &ctl->info);
	if (status)
		goto error;

	/* numa_node. */
	status = rte_swx_ctl_pipeline_numa_node_get(p, &ctl->numa_node);
	if (status)
		goto error;

	/* p. */
	ctl->p = p;

	/* actions. */
	ctl->actions = calloc(ctl->info.n_actions, sizeof(struct action));
	if (!ctl->actions)
		goto error;

	for (i = 0; i < ctl->info.n_actions; i++) {
		struct action *a = &ctl->actions[i];
		uint32_t j;

		/* info. */
		status = rte_swx_ctl_action_info_get(p, i, &a->info);
		if (status)
			goto error;

		/* args. */
		a->args = calloc(a->info.n_args,
				 sizeof(struct rte_swx_ctl_action_arg_info));
		if (!a->args)
			goto error;

		for (j = 0; j < a->info.n_args; j++) {
			status = rte_swx_ctl_action_arg_info_get(p,
								 i,
								 j,
								 &a->args[j]);
			if (status)
				goto error;
		}

		/* data_size. */
		for (j = 0; j < a->info.n_args; j++) {
			struct rte_swx_ctl_action_arg_info *info = &a->args[j];

			a->data_size += info->n_bits;
		}

		a->data_size = (a->data_size + 7) / 8;
	}

	/* tables. */
	ctl->tables = calloc(ctl->info.n_tables, sizeof(struct table));
	if (!ctl->tables)
		goto error;

	for (i = 0; i < ctl->info.n_tables; i++) {
		struct table *t = &ctl->tables[i];

		TAILQ_INIT(&t->entries);
		TAILQ_INIT(&t->pending_add);
		TAILQ_INIT(&t->pending_modify0);
		TAILQ_INIT(&t->pending_modify1);
		TAILQ_INIT(&t->pending_delete);
	}

	for (i = 0; i < ctl->info.n_tables; i++) {
		struct table *t = &ctl->tables[i];
		uint32_t j;

		/* info. */
		status = rte_swx_ctl_table_info_get(p, i, &t->info);
		if (status)
			goto error;

		/* mf. */
		t->mf = calloc(t->info.n_match_fields,
			sizeof(struct rte_swx_ctl_table_match_field_info));
		if (!t->mf)
			goto error;

		for (j = 0; j < t->info.n_match_fields; j++) {
			status = rte_swx_ctl_table_match_field_info_get(p,
				i,
				j,
				&t->mf[j]);
			if (status)
				goto error;
		}

		/* actions. */
		t->actions = calloc(t->info.n_actions,
			sizeof(struct rte_swx_ctl_table_action_info));
		if (!t->actions)
			goto error;

		for (j = 0; j < t->info.n_actions; j++) {
			status = rte_swx_ctl_table_action_info_get(p,
				i,
				j,
				&t->actions[j]);
			if (status ||
			    t->actions[j].action_id >= ctl->info.n_actions)
				goto error;
		}

		/* ops, is_stub. */
		status = rte_swx_ctl_table_ops_get(p, i, &t->ops, &t->is_stub);
		if (status)
			goto error;

		if ((t->is_stub && t->info.n_match_fields) ||
		    (!t->is_stub && !t->info.n_match_fields))
			goto error;

		/* params. */
		status = table_params_get(ctl, i);
		if (status)
			goto error;
	}

	/* ts. */
	status = rte_swx_pipeline_table_state_get(p, &ctl->ts);
	if (status)
		goto error;

	/* ts_next. */
	status = table_state_create(ctl);
	if (status)
		goto error;

	return ctl;

error:
	rte_swx_ctl_pipeline_free(ctl);
	return NULL;
}

int
rte_swx_ctl_pipeline_table_entry_add(struct rte_swx_ctl_pipeline *ctl,
				     const char *table_name,
				     struct rte_swx_table_entry *entry)
{
	struct table *table;
	struct rte_swx_table_entry *new_entry, *existing_entry;
	uint32_t table_id;

	CHECK(ctl, EINVAL);
	CHECK(table_name && table_name[0], EINVAL);

	table = table_find(ctl, table_name);
	CHECK(table, EINVAL);
	table_id = table - ctl->tables;

	CHECK(entry, EINVAL);
	CHECK(!table_entry_check(ctl, table_id, entry, 1, 1), EINVAL);

	new_entry = table_entry_duplicate(ctl, table_id, entry, 1, 1);
	CHECK(new_entry, ENOMEM);

	/* The new entry is found in the table->entries list:
	 * - Add the new entry to the table->pending_modify1 list;
	 * - Move the existing entry from the table->entries list to the
	 *   table->pending_modify0 list.
	 */
	existing_entry = table_entries_find(table, entry);
	if (existing_entry) {
		TAILQ_INSERT_TAIL(&table->pending_modify1,
				  new_entry,
				  node);

		TAILQ_REMOVE(&table->entries,
			     existing_entry,
			     node);

		TAILQ_INSERT_TAIL(&table->pending_modify0,
				  existing_entry,
				  node);

		return 0;
	}

	/* The new entry is found in the table->pending_add list:
	 * - Replace the entry in the table->pending_add list with the new entry
	 *   (and free the replaced entry).
	 */
	existing_entry = table_pending_add_find(table, entry);
	if (existing_entry) {
		TAILQ_INSERT_AFTER(&table->pending_add,
				   existing_entry,
				   new_entry,
				   node);

		TAILQ_REMOVE(&table->pending_add,
			     existing_entry,
			     node);

		table_entry_free(existing_entry);

		return 0;
	}

	/* The new entry is found in the table->pending_modify1 list:
	 * - Replace the entry in the table->pending_modify1 list with the new
	 *   entry (and free the replaced entry).
	 */
	existing_entry = table_pending_modify1_find(table, entry);
	if (existing_entry) {
		TAILQ_INSERT_AFTER(&table->pending_modify1,
				   existing_entry,
				   new_entry,
				   node);

		TAILQ_REMOVE(&table->pending_modify1,
			     existing_entry,
			     node);

		table_entry_free(existing_entry);

		return 0;
	}

	/* The new entry is found in the table->pending_delete list:
	 * - Add the new entry to the table->pending_modify1 list;
	 * - Move the existing entry from the table->pending_delete list to the
	 *   table->pending_modify0 list.
	 */
	existing_entry = table_pending_delete_find(table, entry);
	if (existing_entry) {
		TAILQ_INSERT_TAIL(&table->pending_modify1,
				  new_entry,
				  node);

		TAILQ_REMOVE(&table->pending_delete,
			     existing_entry,
			     node);

		TAILQ_INSERT_TAIL(&table->pending_modify0,
				  existing_entry,
				  node);

		return 0;
	}

	/* The new entry is not found in any of the above lists:
	 * - Add the new entry to the table->pending_add list.
	 */
	TAILQ_INSERT_TAIL(&table->pending_add, new_entry, node);

	return 0;
}

int
rte_swx_ctl_pipeline_table_entry_delete(struct rte_swx_ctl_pipeline *ctl,
					const char *table_name,
					struct rte_swx_table_entry *entry)
{
	struct table *table;
	struct rte_swx_table_entry *existing_entry;
	uint32_t table_id;

	CHECK(ctl, EINVAL);

	CHECK(table_name && table_name[0], EINVAL);
	table = table_find(ctl, table_name);
	CHECK(table, EINVAL);
	table_id = table - ctl->tables;

	CHECK(entry, EINVAL);
	CHECK(!table_entry_check(ctl, table_id, entry, 1, 0), EINVAL);

	/* The entry is found in the table->entries list:
	 * - Move the existing entry from the table->entries list to to the
	 *   table->pending_delete list.
	 */
	existing_entry = table_entries_find(table, entry);
	if (existing_entry) {
		TAILQ_REMOVE(&table->entries,
			     existing_entry,
			     node);

		TAILQ_INSERT_TAIL(&table->pending_delete,
				  existing_entry,
				  node);

		return 0;
	}

	/* The entry is found in the table->pending_add list:
	 * - Remove the entry from the table->pending_add list and free it.
	 */
	existing_entry = table_pending_add_find(table, entry);
	if (existing_entry) {
		TAILQ_REMOVE(&table->pending_add,
			     existing_entry,
			     node);

		table_entry_free(existing_entry);
	}

	/* The entry is found in the table->pending_modify1 list:
	 * - Free the entry in the table->pending_modify1 list;
	 * - Move the existing entry from the table->pending_modify0 list to the
	 *   table->pending_delete list.
	 */
	existing_entry = table_pending_modify1_find(table, entry);
	if (existing_entry) {
		struct rte_swx_table_entry *real_existing_entry;

		TAILQ_REMOVE(&table->pending_modify1,
			     existing_entry,
			     node);

		table_entry_free(existing_entry);

		real_existing_entry = table_pending_modify0_find(table, entry);
		CHECK(real_existing_entry, EINVAL); /* Coverity. */

		TAILQ_REMOVE(&table->pending_modify0,
			     real_existing_entry,
			     node);

		TAILQ_INSERT_TAIL(&table->pending_delete,
				  real_existing_entry,
				  node);

		return 0;
	}

	/* The entry is found in the table->pending_delete list:
	 * - Do nothing: the existing entry is already in the
	 *   table->pending_delete list, i.e. already marked for delete, so
	 *   simply keep it there as it is.
	 */

	/* The entry is not found in any of the above lists:
	 * - Do nothing: no existing entry to delete.
	 */

	return 0;
}

int
rte_swx_ctl_pipeline_table_default_entry_add(struct rte_swx_ctl_pipeline *ctl,
					     const char *table_name,
					     struct rte_swx_table_entry *entry)
{
	struct table *table;
	struct rte_swx_table_entry *new_entry;
	uint32_t table_id;

	CHECK(ctl, EINVAL);

	CHECK(table_name && table_name[0], EINVAL);
	table = table_find(ctl, table_name);
	CHECK(table, EINVAL);
	table_id = table - ctl->tables;
	CHECK(!table->info.default_action_is_const, EINVAL);

	CHECK(entry, EINVAL);
	CHECK(!table_entry_check(ctl, table_id, entry, 0, 1), EINVAL);

	new_entry = table_entry_duplicate(ctl, table_id, entry, 0, 1);
	CHECK(new_entry, ENOMEM);

	table_pending_default_free(table);

	table->pending_default = new_entry;
	return 0;
}


static void
table_entry_list_free(struct rte_swx_table_entry_list *list)
{
	for ( ; ; ) {
		struct rte_swx_table_entry *entry;

		entry = TAILQ_FIRST(list);
		if (!entry)
			break;

		TAILQ_REMOVE(list, entry, node);
		table_entry_free(entry);
	}
}

static int
table_entry_list_duplicate(struct rte_swx_ctl_pipeline *ctl,
			   uint32_t table_id,
			   struct rte_swx_table_entry_list *dst,
			   struct rte_swx_table_entry_list *src)
{
	struct rte_swx_table_entry *src_entry;

	TAILQ_FOREACH(src_entry, src, node) {
		struct rte_swx_table_entry *dst_entry;

		dst_entry = table_entry_duplicate(ctl, table_id, src_entry, 1, 1);
		if (!dst_entry)
			goto error;

		TAILQ_INSERT_TAIL(dst, dst_entry, node);
	}

	return 0;

error:
	table_entry_list_free(dst);
	return -ENOMEM;
}

/* This commit stage contains all the operations that can fail; in case ANY of
 * them fails for ANY table, ALL of them are rolled back for ALL the tables.
 */
static int
table_rollfwd0(struct rte_swx_ctl_pipeline *ctl,
	       uint32_t table_id,
	       uint32_t after_swap)
{
	struct table *table = &ctl->tables[table_id];
	struct rte_swx_table_state *ts = &ctl->ts[table_id];
	struct rte_swx_table_state *ts_next = &ctl->ts_next[table_id];

	if (table->is_stub || !table_is_update_pending(table, 0))
		return 0;

	/*
	 * Current table supports incremental update.
	 */
	if (table->ops.add) {
		/* Reset counters. */
		table->n_add = 0;
		table->n_modify = 0;
		table->n_delete = 0;

		/* Add pending rules. */
		struct rte_swx_table_entry *entry;

		TAILQ_FOREACH(entry, &table->pending_add, node) {
			int status;

			status = table->ops.add(ts_next->obj, entry);
			if (status)
				return status;

			table->n_add++;
		}

		/* Modify pending rules. */
		TAILQ_FOREACH(entry, &table->pending_modify1, node) {
			int status;

			status = table->ops.add(ts_next->obj, entry);
			if (status)
				return status;

			table->n_modify++;
		}

		/* Delete pending rules. */
		TAILQ_FOREACH(entry, &table->pending_delete, node) {
			int status;

			status = table->ops.del(ts_next->obj, entry);
			if (status)
				return status;

			table->n_delete++;
		}

		return 0;
	}

	/*
	 * Current table does NOT support incremental update.
	 */
	if (!after_swap) {
		struct rte_swx_table_entry_list list;
		int status;

		/* Create updated list of entries included. */
		TAILQ_INIT(&list);

		status = table_entry_list_duplicate(ctl,
						    table_id,
						    &list,
						    &table->entries);
		if (status)
			goto error;

		status = table_entry_list_duplicate(ctl,
						    table_id,
						    &list,
						    &table->pending_add);
		if (status)
			goto error;

		status = table_entry_list_duplicate(ctl,
						    table_id,
						    &list,
						    &table->pending_modify1);
		if (status)
			goto error;

		/* Create new table object with the updates included. */
		ts_next->obj = table->ops.create(&table->params,
						 &list,
						 table->info.args,
						 ctl->numa_node);
		if (!ts_next->obj) {
			status = -ENODEV;
			goto error;
		}

		table_entry_list_free(&list);

		return 0;

error:
		table_entry_list_free(&list);
		return status;
	}

	/* Free the old table object. */
	if (ts_next->obj && table->ops.free)
		table->ops.free(ts_next->obj);

	/* Copy over the new table object. */
	ts_next->obj = ts->obj;

	return 0;
}

/* This commit stage contains all the operations that cannot fail. They are
 * executed only if the previous stage was successful for ALL the tables. Hence,
 * none of these operations has to be rolled back for ANY table.
 */
static void
table_rollfwd1(struct rte_swx_ctl_pipeline *ctl, uint32_t table_id)
{
	struct table *table = &ctl->tables[table_id];
	struct rte_swx_table_state *ts_next = &ctl->ts_next[table_id];
	struct action *a;
	uint8_t *action_data;
	uint64_t action_id;

	/* Copy the pending default entry. */
	if (!table->pending_default)
		return;

	action_id = table->pending_default->action_id;
	action_data = table->pending_default->action_data;
	a = &ctl->actions[action_id];

	memcpy(ts_next->default_action_data,
	       action_data,
	       a->data_size);

	ts_next->default_action_id = action_id;
}

/* This last commit stage is simply finalizing a successful commit operation.
 * This stage is only executed if all the previous stages were successful. This
 * stage cannot fail.
 */
static void
table_rollfwd2(struct rte_swx_ctl_pipeline *ctl, uint32_t table_id)
{
	struct table *table = &ctl->tables[table_id];

	/* Move all the pending add entries to the table, as they are now part
	 * of the table.
	 */
	table_pending_add_admit(table);

	/* Move all the pending modify1 entries to table, are they are now part
	 * of the table. Free up all the pending modify0 entries, as they are no
	 * longer part of the table.
	 */
	table_pending_modify1_admit(table);
	table_pending_modify0_free(table);

	/* Free up all the pending delete entries, as they are no longer part of
	 * the table.
	 */
	table_pending_delete_free(table);

	/* Free up the pending default entry, as it is now part of the table. */
	table_pending_default_free(table);
}

/* The rollback stage is only executed when the commit failed, i.e. ANY of the
 * commit operations that can fail did fail for ANY table. It reverts ALL the
 * tables to their state before the commit started, as if the commit never
 * happened.
 */
static void
table_rollback(struct rte_swx_ctl_pipeline *ctl, uint32_t table_id)
{
	struct table *table = &ctl->tables[table_id];
	struct rte_swx_table_state *ts_next = &ctl->ts_next[table_id];

	if (table->is_stub || !table_is_update_pending(table, 0))
		return;

	if (table->ops.add) {
		struct rte_swx_table_entry *entry;

		/* Add back all the entries that were just deleted. */
		TAILQ_FOREACH(entry, &table->pending_delete, node) {
			if (!table->n_delete)
				break;

			table->ops.add(ts_next->obj, entry);
			table->n_delete--;
		}

		/* Add back the old copy for all the entries that were just
		 * modified.
		 */
		TAILQ_FOREACH(entry, &table->pending_modify0, node) {
			if (!table->n_modify)
				break;

			table->ops.add(ts_next->obj, entry);
			table->n_modify--;
		}

		/* Delete all the entries that were just added. */
		TAILQ_FOREACH(entry, &table->pending_add, node) {
			if (!table->n_add)
				break;

			table->ops.del(ts_next->obj, entry);
			table->n_add--;
		}
	} else {
		struct rte_swx_table_state *ts = &ctl->ts[table_id];

		/* Free the new table object, as update was cancelled. */
		if (ts_next->obj && table->ops.free)
			table->ops.free(ts_next->obj);

		/* Reinstate the old table object. */
		ts_next->obj = ts->obj;
	}
}

/* This stage is conditionally executed (as instructed by the user) after a
 * failed commit operation to remove ALL the pending work for ALL the tables.
 */
static void
table_abort(struct rte_swx_ctl_pipeline *ctl, uint32_t table_id)
{
	struct table *table = &ctl->tables[table_id];

	/* Free up all the pending add entries, as none of them is part of the
	 * table.
	 */
	table_pending_add_free(table);

	/* Free up all the pending modify1 entries, as none of them made it to
	 * the table. Add back all the pending modify0 entries, as none of them
	 * was deleted from the table.
	 */
	table_pending_modify1_free(table);
	table_pending_modify0_admit(table);

	/* Add back all the pending delete entries, as none of them was deleted
	 * from the table.
	 */
	table_pending_delete_admit(table);

	/* Free up the pending default entry, as it is no longer going to be
	 * added to the table.
	 */
	table_pending_default_free(table);
}

int
rte_swx_ctl_pipeline_commit(struct rte_swx_ctl_pipeline *ctl, int abort_on_fail)
{
	struct rte_swx_table_state *ts;
	int status = 0;
	uint32_t i;

	CHECK(ctl, EINVAL);

	/* Operate the changes on the current ts_next before it becomes the new
	 * ts.
	 */
	for (i = 0; i < ctl->info.n_tables; i++) {
		status = table_rollfwd0(ctl, i, 0);
		if (status)
			goto rollback;
	}

	for (i = 0; i < ctl->info.n_tables; i++)
		table_rollfwd1(ctl, i);

	/* Swap the table state for the data plane. The current ts and ts_next
	 * become the new ts_next and ts, respectively.
	 */
	rte_swx_pipeline_table_state_set(ctl->p, ctl->ts_next);
	usleep(100);
	ts = ctl->ts;
	ctl->ts = ctl->ts_next;
	ctl->ts_next = ts;

	/* Operate the changes on the current ts_next, which is the previous ts.
	 */
	for (i = 0; i < ctl->info.n_tables; i++) {
		table_rollfwd0(ctl, i, 1);
		table_rollfwd1(ctl, i);
		table_rollfwd2(ctl, i);
	}

	return 0;

rollback:
	for (i = 0; i < ctl->info.n_tables; i++) {
		table_rollback(ctl, i);
		if (abort_on_fail)
			table_abort(ctl, i);
	}

	return status;
}

void
rte_swx_ctl_pipeline_abort(struct rte_swx_ctl_pipeline *ctl)
{
	uint32_t i;

	if (!ctl)
		return;

	for (i = 0; i < ctl->info.n_tables; i++)
		table_abort(ctl, i);
}

static int
token_is_comment(const char *token)
{
	if ((token[0] == '#') ||
	    (token[0] == ';') ||
	    ((token[0] == '/') && (token[1] == '/')))
		return 1; /* TRUE. */

	return 0; /* FALSE. */
}

#define RTE_SWX_CTL_ENTRY_TOKENS_MAX 256

struct rte_swx_table_entry *
rte_swx_ctl_pipeline_table_entry_read(struct rte_swx_ctl_pipeline *ctl,
				      const char *table_name,
				      const char *string,
				      int *is_blank_or_comment)
{
	char *token_array[RTE_SWX_CTL_ENTRY_TOKENS_MAX], **tokens;
	struct table *table;
	struct action *action;
	struct rte_swx_table_entry *entry = NULL;
	char *s0 = NULL, *s;
	uint32_t n_tokens = 0, arg_offset = 0, i;
	int blank_or_comment = 0;

	/* Check input arguments. */
	if (!ctl)
		goto error;

	if (!table_name || !table_name[0])
		goto error;

	table = table_find(ctl, table_name);
	if (!table)
		goto error;

	if (!string || !string[0])
		goto error;

	/* Memory allocation. */
	s0 = strdup(string);
	if (!s0)
		goto error;

	entry = table_entry_alloc(table);
	if (!entry)
		goto error;

	/* Parse the string into tokens. */
	for (s = s0; ; ) {
		char *token;

		token = strtok_r(s, " \f\n\r\t\v", &s);
		if (!token || token_is_comment(token))
			break;

		if (n_tokens >= RTE_SWX_CTL_ENTRY_TOKENS_MAX)
			goto error;

		token_array[n_tokens] = token;
		n_tokens++;
	}

	if (!n_tokens) {
		blank_or_comment = 1;
		goto error;
	}

	tokens = token_array;

	/*
	 * Match.
	 */
	if (n_tokens && strcmp(tokens[0], "match"))
		goto action;

	if (n_tokens < 1 + table->info.n_match_fields)
		goto error;

	for (i = 0; i < table->info.n_match_fields; i++) {
		struct rte_swx_ctl_table_match_field_info *mf = &table->mf[i];
		char *mf_val = tokens[1 + i], *mf_mask = NULL;
		uint64_t val, mask = UINT64_MAX;
		uint32_t offset = (mf->offset - table->mf[0].offset) / 8;

		/*
		 * Mask.
		 */
		mf_mask = strchr(mf_val, '/');
		if (mf_mask) {
			*mf_mask = 0;
			mf_mask++;

			/* Parse. */
			mask = strtoull(mf_mask, &mf_mask, 0);
			if (mf_mask[0])
				goto error;

			/* Endianness conversion. */
			if (mf->is_header)
				mask = field_hton(mask, mf->n_bits);
		}

		/* Copy to entry. */
		if (entry->key_mask)
			memcpy(&entry->key_mask[offset],
			       (uint8_t *)&mask,
			       mf->n_bits / 8);

		/*
		 * Value.
		 */
		/* Parse. */
		val = strtoull(mf_val, &mf_val, 0);
		if (mf_val[0])
			goto error;

		/* Endianness conversion. */
		if (mf->is_header)
			val = field_hton(val, mf->n_bits);

		/* Copy to entry. */
		memcpy(&entry->key[offset],
		       (uint8_t *)&val,
		       mf->n_bits / 8);
	}

	tokens += 1 + table->info.n_match_fields;
	n_tokens -= 1 + table->info.n_match_fields;

	/*
	 * Match priority.
	 */
	if (n_tokens && !strcmp(tokens[0], "priority")) {
		char *priority = tokens[1];
		uint32_t val;

		if (n_tokens < 2)
			goto error;

		/* Parse. */
		val = strtoul(priority, &priority, 0);
		if (priority[0])
			goto error;

		/* Copy to entry. */
		entry->key_priority = val;

		tokens += 2;
		n_tokens -= 2;
	}

	/*
	 * Action.
	 */
action:
	if (n_tokens && strcmp(tokens[0], "action"))
		goto other;

	if (n_tokens < 2)
		goto error;

	action = action_find(ctl, tokens[1]);
	if (!action)
		goto error;

	if (n_tokens < 2 + action->info.n_args * 2)
		goto error;

	/* action_id. */
	entry->action_id = action - ctl->actions;

	/* action_data. */
	for (i = 0; i < action->info.n_args; i++) {
		struct rte_swx_ctl_action_arg_info *arg = &action->args[i];
		char *arg_name, *arg_val;
		uint64_t val;
		int is_nbo = 0;

		arg_name = tokens[2 + i * 2];
		arg_val = tokens[2 + i * 2 + 1];

		if (strcmp(arg_name, arg->name) ||
		    (strlen(arg_val) < 4) ||
		    ((arg_val[0] != 'H') && (arg_val[0] != 'N')) ||
		    (arg_val[1] != '(') ||
		    (arg_val[strlen(arg_val) - 1] != ')'))
			goto error;

		if (arg_val[0] == 'N')
			is_nbo = 1;

		arg_val[strlen(arg_val) - 1] = 0; /* Remove the ')'. */
		arg_val += 2; /* Remove the "H(" or "N(". */

		val = strtoull(arg_val, &arg_val, 0);
		if (arg_val[0])
			goto error;

		/* Endianness conversion. */
		if (is_nbo)
			val = field_hton(val, arg->n_bits);

		/* Copy to entry. */
		memcpy(&entry->action_data[arg_offset],
		       (uint8_t *)&val,
		       arg->n_bits / 8);

		arg_offset += arg->n_bits / 8;
	}

	tokens += 2 + action->info.n_args * 2;
	n_tokens -= 2 + action->info.n_args * 2;

other:
	if (n_tokens)
		goto error;

	free(s0);
	return entry;

error:
	table_entry_free(entry);
	free(s0);
	if (is_blank_or_comment)
		*is_blank_or_comment = blank_or_comment;
	return NULL;
}

static void
table_entry_printf(FILE *f,
		   struct rte_swx_ctl_pipeline *ctl,
		   struct table *table,
		   struct rte_swx_table_entry *entry)
{
	struct action *action = &ctl->actions[entry->action_id];
	uint32_t i;

	fprintf(f, "match ");
	for (i = 0; i < table->params.key_size; i++)
		fprintf(f, "%02x", entry->key[i]);

	if (entry->key_mask) {
		fprintf(f, "/");
		for (i = 0; i < table->params.key_size; i++)
			fprintf(f, "%02x", entry->key_mask[i]);
	}

	fprintf(f, " priority %u", entry->key_priority);

	fprintf(f, " action %s ", action->info.name);
	for (i = 0; i < action->data_size; i++)
		fprintf(f, "%02x", entry->action_data[i]);

	fprintf(f, "\n");
}

int
rte_swx_ctl_pipeline_table_fprintf(FILE *f,
				   struct rte_swx_ctl_pipeline *ctl,
				   const char *table_name)
{
	struct table *table;
	struct rte_swx_table_entry *entry;
	uint32_t n_entries = 0, i;

	if (!f || !ctl || !table_name || !table_name[0])
		return -EINVAL;

	table = table_find(ctl, table_name);
	if (!table)
		return -EINVAL;

	/* Table. */
	fprintf(f, "# Table %s: key size %u bytes, key offset %u, key mask [",
		table->info.name,
		table->params.key_size,
		table->params.key_offset);

	for (i = 0; i < table->params.key_size; i++)
		fprintf(f, "%02x", table->params.key_mask0[i]);

	fprintf(f, "], action data size %u bytes\n",
		table->params.action_data_size);

	/* Table entries. */
	TAILQ_FOREACH(entry, &table->entries, node) {
		table_entry_printf(f, ctl, table, entry);
		n_entries++;
	}

	TAILQ_FOREACH(entry, &table->pending_modify0, node) {
		table_entry_printf(f, ctl, table, entry);
		n_entries++;
	}

	TAILQ_FOREACH(entry, &table->pending_delete, node) {
		table_entry_printf(f, ctl, table, entry);
		n_entries++;
	}

	fprintf(f, "# Table %s currently has %u entries.\n",
		table_name,
		n_entries);
	return 0;
}
