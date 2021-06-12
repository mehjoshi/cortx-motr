/* -*- C -*- */
/*
 * Copyright (c) 2017-2020 Seagate Technology LLC and/or its Affiliates
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * For any questions about this software or licensing,
 * please email opensource@seagate.com or cortx-questions@seagate.com.
 *
 */

#define M0_TRACE_SUBSYSTEM M0_TRACE_SUBSYS_UT
#include "lib/trace.h"
#include "lib/coroutine.h"
#include "lib/string.h"
#include "ut/ut.h"
#include "be/op.h"
#include "reqh/reqh.h"		/* m0_reqh_fop_handle */
#include "fop/fom.h"		/* m0_fom_phase_set */
#include "fop/fom_generic.h"	/* M0_FOPH_FINISH */
#include "fop/fom_simple.h"

#define F M0_CO_FRAME_DATA

extern struct m0_reqh *m0_ut__reqh_init(void);
extern        void     m0_ut__reqh_fini(void);

enum coroutine2_state {
	C2_INIT   = M0_FOM_PHASE_INIT,
	C2_FINISH = M0_FOM_PHASE_FINISH,
	C2_NR,
};

static struct m0_sm_state_descr coroutine2_states[C2_NR] = {
#define _S(name, flags, allowed)      \
	[name] = {                    \
		.sd_flags   = flags,  \
		.sd_name    = #name,  \
		.sd_allowed = allowed \
	}

	_S(C2_INIT,   M0_SDF_INITIAL, M0_BITS(C2_INIT, C2_FINISH)),
	_S(C2_FINISH, M0_SDF_TERMINAL, 0),
#undef _S
};

static struct m0_sm_conf coroutine2_conf = {
	.scf_name      = "coroutine2_states",
	.scf_nr_states = ARRAY_SIZE(coroutine2_states),
	.scf_state     = coroutine2_states,
};

struct test_tree {
	int insert;
	int delete;
};

static void crud(struct m0_co_context *context,
		 struct test_tree *tree, int k, int v);
static void insert(struct m0_co_context *context,
		   struct test_tree *tree, int k, int v);
static void delete(struct m0_co_context *context,
		   struct test_tree *tree, int k);

static struct m0_semaphore ready;
static struct m0_semaphore insert1;
static struct m0_semaphore insert2;
static struct m0_semaphore delete1;
static struct m0_semaphore delete2;

static struct m0_fom_simple simple_fom;
static struct m0_co_context fom_context = {};
static struct test_tree     fom_tree = {};
static struct m0_co_op      fom_op = {};


static void coroutine_fom_run(void)
{
	m0_semaphore_down(&insert1);
	m0_co_op_done(&fom_op);

	m0_semaphore_down(&insert2);
	m0_co_op_done(&fom_op);

	m0_semaphore_down(&delete1);
	m0_co_op_done(&fom_op);

	m0_semaphore_down(&delete2);
	m0_co_op_done(&fom_op);

	m0_semaphore_down(&ready);
}

static int coroutine_fom_tick(struct m0_fom *fom, int *x, int *__unused)
{
	int key = 1;
	int val = 2;
	int rc;

	m0_co_op_reset(&fom_op);

	M0_CO_START(&fom_context);
	crud(&fom_context, &fom_tree, key, val);
	rc = M0_CO_END(&fom_context);

	if (rc == -EAGAIN)
		return m0_co_op_tick_ret(&fom_op, fom, m0_fom_phase(fom));

	m0_semaphore_up(&ready);
	return -1;
}

static void crud(struct m0_co_context *context,
		struct test_tree *tree, int k, int v)
{
	M0_CO_REENTER(context);

	M0_LOG(M0_DEBUG, "tree=%p tree.delete=%d, tree.insert=%d k=%d",
	       tree, tree->delete, tree->insert, k);

	M0_CO_FUN(context, insert(context, tree, k, v));

	M0_CO_FUN(context, delete(context, tree, k));

	M0_UT_ASSERT(tree->delete + tree->insert == 4);
}

static void insert(struct m0_co_context *context,
		   struct test_tree *tree, int k, int v)
{
	M0_CO_REENTER(context,
		      int         rc;
		      char        c;
		      long long   i;
		);
	F(rc) = 0;
	F(i) = 0x5011D57A7E;
	F(c) = '8';

	M0_LOG(M0_DEBUG, "tree=%p tree.delete=%d, tree.insert=%d k=%d v=%d",
	       tree, tree->delete, tree->insert, k, v);

	M0_LOG(M0_DEBUG, "yield");
	tree->insert++;
	m0_co_op_active(&fom_op);
	m0_semaphore_up(&insert1);
	M0_CO_YIELD(context);

	M0_LOG(M0_DEBUG, "yield");
	tree->insert++;
	m0_co_op_active(&fom_op);
	m0_semaphore_up(&insert2);
	M0_CO_YIELD(context);
}

static void delete(struct m0_co_context *context,
		   struct test_tree *tree, int k)
{
	M0_CO_REENTER(context);

	M0_LOG(M0_DEBUG, "tree=%p tree.delete=%d, tree.insert=%d",
	       tree, tree->delete, tree->insert);

	M0_LOG(M0_DEBUG, "yield");
	tree->delete++;
	m0_co_op_active(&fom_op);
	m0_semaphore_up(&delete1);
	M0_CO_YIELD(context);

	M0_LOG(M0_DEBUG, "yield");
	tree->delete++;
	m0_co_op_active(&fom_op);
	m0_semaphore_up(&delete2);
	M0_CO_YIELD(context);
}

static void test_run(void)
{
	struct m0_reqh *reqh = m0_ut__reqh_init();
	(void) reqh;

	M0_SET0(&simple_fom);
	M0_FOM_SIMPLE_POST(&simple_fom, reqh, &coroutine2_conf,
			   &coroutine_fom_tick, NULL, NULL, 1);
	coroutine_fom_run();
	m0_ut__reqh_fini();
}

void m0_test_coroutine2(void)
{
	int rc;

	m0_semaphore_init(&ready, 0);

	m0_semaphore_init(&insert1, 0);
	m0_semaphore_init(&insert2, 0);
	m0_semaphore_init(&delete1, 0);
	m0_semaphore_init(&delete2, 0);

	m0_co_op_init(&fom_op);

	rc = m0_co_context_init(&fom_context);
	M0_UT_ASSERT(rc == 0);


	test_run();

	m0_co_context_fini(&fom_context);
	m0_co_op_fini(&fom_op);

	m0_semaphore_init(&delete2, 0);
	m0_semaphore_init(&delete1, 0);
	m0_semaphore_init(&insert2, 0);
	m0_semaphore_init(&insert1, 0);

	m0_semaphore_init(&ready, 0);
}

/*
 *  Local variables:
 *  c-indentation-style: "K&R"
 *  c-basic-offset: 8
 *  tab-width: 8
 *  fill-column: 80
 *  scroll-step: 1
 *  End:
 */
/*
 * vim: tabstop=8 shiftwidth=8 noexpandtab textwidth=80 nowrap
 */
