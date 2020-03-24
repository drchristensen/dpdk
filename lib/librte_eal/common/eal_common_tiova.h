/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2020 IBM Corporation
 */

#ifndef EAL_COMMON_TIOVA_H_
#define EAL_COMMON_TIOVA_H_

#include <stdint.h>
#include <stdbool.h>

uint64_t iova_alloc(const void *, const size_t);
int iova_free(const void *, const size_t);
int iova_free_init(const void *, const size_t);
uint64_t iova_search(const void *);
int iova_init(void);

#endif /* EAL_COMMON_TIOVA_H_ */
