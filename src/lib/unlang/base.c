/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @file unlang/base.c
 * @brief Base, utility functions for the unlang library.
 *
 * @copyright 2019 The FreeRADIUS server project
 */
RCSID("$Id$")

#include "unlang_priv.h"

/** Different operations the interpreter can execute
 */
unlang_op_t unlang_ops[UNLANG_TYPE_MAX];

/** Return whether a section has unlang data associated with it
 *
 * @param[in] cs	to check.
 * @return
 *	- true if it has data.
 *	- false if it doesn't have data.
 */
bool unlang_section(CONF_SECTION *cs)
{
	return (cf_data_find(cs, unlang_group_t, NULL) != NULL);
}

fr_hash_table_t *unlang_op_table = NULL;

/** Register an operation with the interpreter
 *
 * The main purpose of this registration API is to avoid intermixing the xlat,
 * condition, map APIs with the interpreter, i.e. the callbacks needed for that
 * functionality can be in their own source files, and we don't need to include
 * supporting types and function declarations in the interpreter.
 *
 * @param[in] op		#unlang_op_t to register.
 */
void unlang_register(unlang_op_t *op)
{
	fr_assert(op->type < UNLANG_TYPE_MAX);	/* Unlang max isn't a valid type */
	fr_assert(unlang_op_table != NULL);

	memcpy(&unlang_ops[op->type], op, sizeof(unlang_ops[op->type]));

	/*
	 *	Some instruction types are internal, and are not real keywords.
	 */
	if ((op->flag & UNLANG_OP_FLAG_INTERNAL) != 0) return;

	MEM(fr_hash_table_insert(unlang_op_table, &unlang_ops[op->type]));
}

static TALLOC_CTX *unlang_ctx = NULL;

static uint32_t op_hash(void const *data)
{
	unlang_op_t const *a = data;

	return fr_hash_string(a->name);
}

static int8_t op_cmp(void const *one, void const *two)
{
	unlang_op_t const *a = one;
	unlang_op_t const *b = two;

	return CMP(strcmp(a->name, b->name), 0);
}

static int _unlang_global_free(UNUSED void *uctx)
{
	TALLOC_FREE(unlang_ctx);
	unlang_op_table = NULL;

	return 0;
}

static int _unlang_global_init(UNUSED void *uctx)
{
	unlang_ctx = talloc_init("unlang");
	if (!unlang_ctx) return -1;

	unlang_op_table = fr_hash_table_alloc(unlang_ctx, op_hash, op_cmp, NULL);
	if (!unlang_op_table) goto fail;

	/*
	 *	Explicitly initialise the xlat tree, and perform dictionary lookups.
	 */
	if (xlat_global_init() < 0) {
	fail:
		TALLOC_FREE(unlang_ctx);

		memset(unlang_ops, 0, sizeof(unlang_ops));
		return -1;
	}

	/*
	 *	Initialise global maps
	 */
	if (map_global_init() < 0) goto fail;

	unlang_interpret_init_global(unlang_ctx);

	/*
	 *	Operations which can fail, and which require cleanup.
	 */
	if (unlang_subrequest_op_init() < 0) goto fail;

	/*
	 *	Register operations for the default keywords.  The
	 *	operations listed below cannot fail, and do not
	 *	require cleanup.
	 */
	unlang_compile_init(unlang_ctx);
	unlang_condition_init();
	unlang_finally_init();
	unlang_foreach_init();
	unlang_function_init();
	unlang_group_init();
	unlang_load_balance_init();
	unlang_map_init();
	unlang_module_init();
	unlang_parallel_init();
	unlang_return_init();
	unlang_detach_init();
	unlang_switch_init();
	unlang_call_init();
	unlang_caller_init();
	unlang_tmpl_init();
	unlang_edit_init();
	unlang_timeout_init();
	unlang_limit_init();
	unlang_transaction_init();
	unlang_try_init();
	unlang_catch_init();

	return 0;
}

int unlang_global_init(void)
{
	int ret;
	fr_atexit_global_once_ret(&ret, _unlang_global_init, _unlang_global_free, NULL);
	return ret;
}
