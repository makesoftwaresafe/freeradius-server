#pragma once
/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @file io/ring_buffer.h
 * @brief Ring buffers
 *
 * @copyright 2016 The FreeRADIUS Server Project
 * @copyright 2016 Alan DeKok (aland@freeradius.org)
 */
RCSIDH(ring_buffer_h, "$Id$")

#include <freeradius-devel/util/talloc.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fr_ring_buffer_s fr_ring_buffer_t;

fr_ring_buffer_t	*fr_ring_buffer_create(TALLOC_CTX *ctx, size_t size);

uint8_t			*fr_ring_buffer_reserve(fr_ring_buffer_t *rb, size_t size) CC_HINT(nonnull);

uint8_t			*fr_ring_buffer_alloc(fr_ring_buffer_t *rb, size_t size);

int			fr_ring_buffer_start(fr_ring_buffer_t *dst, uint8_t **p_start, size_t *p_size);

int			fr_ring_buffer_free(fr_ring_buffer_t *rb, size_t size) CC_HINT(nonnull);

int			fr_ring_buffer_close(fr_ring_buffer_t *rb) CC_HINT(nonnull);

size_t			fr_ring_buffer_size(fr_ring_buffer_t *rb) CC_HINT(nonnull);

size_t 			fr_ring_buffer_used(fr_ring_buffer_t *rb) CC_HINT(nonnull);

void			fr_ring_buffer_debug(FILE *fp, fr_ring_buffer_t *rb) CC_HINT(nonnull);

#ifdef __cplusplus
}
#endif
