/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2019-2021 Broadcom
 * All rights reserved.
 */

#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_malloc.h>
#include <rte_log.h>
#include <rte_alarm.h>
#include "bnxt.h"
#include "bnxt_ulp.h"
#include "bnxt_tf_common.h"
#include "ulp_ha_mgr.h"
#include "ulp_flow_db.h"

/* Local only MACROs and defines that aren't exported */
#define ULP_HA_TIMER_THREAD	(1 << 0)
#define ULP_HA_TIMER_IS_RUNNING(info) (!!((info)->flags & ULP_HA_TIMER_THREAD))
#define ULP_HA_TIMER_SEC 1
#define ULP_HA_WAIT_TIME (MS_PER_S / 10)
#define ULP_HA_WAIT_TIMEOUT (MS_PER_S * 2)

#define ULP_HA_IF_TBL_DIR	TF_DIR_RX
#define ULP_HA_IF_TBL_TYPE	TF_IF_TBL_TYPE_PROF_PARIF_ERR_ACT_REC_PTR
#define ULP_HA_IF_TBL_IDX 10

static void ulp_ha_mgr_timer_cancel(struct bnxt_ulp_context *ulp_ctx);
static int32_t ulp_ha_mgr_timer_start(void);
static void ulp_ha_mgr_timer_cb(void *arg);
static int32_t ulp_ha_mgr_app_type_set(struct bnxt_ulp_context *ulp_ctx,
				enum ulp_ha_mgr_app_type app_type);
static int32_t
ulp_ha_mgr_region_set(struct bnxt_ulp_context *ulp_ctx,
		      enum ulp_ha_mgr_region region);
static int32_t
ulp_ha_mgr_state_set(struct bnxt_ulp_context *ulp_ctx,
		     enum ulp_ha_mgr_state state);

static int32_t
ulp_ha_mgr_state_set(struct bnxt_ulp_context *ulp_ctx,
		     enum ulp_ha_mgr_state state)
{
	struct tf_set_if_tbl_entry_parms set_parms = { 0 };
	struct tf *tfp;
	uint32_t val = 0;
	int32_t rc = 0;

	if (ulp_ctx == NULL) {
		BNXT_TF_DBG(ERR, "Invalid parms in state get.\n");
		return -EINVAL;
	}
	tfp = bnxt_ulp_cntxt_tfp_get(ulp_ctx, BNXT_ULP_SHARED_SESSION_NO);
	if (tfp == NULL) {
		BNXT_TF_DBG(ERR, "Unable to get the TFP.\n");
		return -EINVAL;
	}

	val = (uint32_t)state;

	set_parms.dir = ULP_HA_IF_TBL_DIR;
	set_parms.type = ULP_HA_IF_TBL_TYPE;
	set_parms.data = (uint8_t *)&val;
	set_parms.data_sz_in_bytes = sizeof(val);
	set_parms.idx = ULP_HA_IF_TBL_IDX;

	rc = tf_set_if_tbl_entry(tfp, &set_parms);
	if (rc)
		BNXT_TF_DBG(ERR, "Failed to write the HA state\n");

	return rc;
}

static int32_t
ulp_ha_mgr_region_set(struct bnxt_ulp_context *ulp_ctx,
		      enum ulp_ha_mgr_region region)
{
	struct bnxt_ulp_ha_mgr_info *ha_info;

	if (ulp_ctx == NULL) {
		BNXT_TF_DBG(ERR, "Invalid params in ha region get.\n");
		return -EINVAL;
	}

	ha_info = bnxt_ulp_cntxt_ptr2_ha_info_get(ulp_ctx);
	if (ha_info == NULL) {
		BNXT_TF_DBG(ERR, "Unable to get ha info\n");
		return -EINVAL;
	}
	ha_info->region = region;

	return 0;
}

static int32_t
ulp_ha_mgr_app_type_set(struct bnxt_ulp_context *ulp_ctx,
			enum ulp_ha_mgr_app_type app_type)
{
	struct bnxt_ulp_ha_mgr_info *ha_info;

	if (ulp_ctx == NULL) {
		BNXT_TF_DBG(ERR, "Invalid Parms.\n");
		return -EINVAL;
	}

	ha_info = bnxt_ulp_cntxt_ptr2_ha_info_get(ulp_ctx);
	if (ha_info == NULL) {
		BNXT_TF_DBG(ERR, "Unable to get the ha info.\n");
		return -EINVAL;
	}
	ha_info->app_type = app_type;

	return 0;
}

/*
 * When a secondary opens, the timer is started and periodically checks for a
 * close of the primary (state moved to SEC_TIMER_COPY).
 * In SEC_TIMER_COPY:
 * - The flow db must be locked to prevent flows from being added to the high
 *   region during a move.
 * - Move the high entries to low
 * - Set the region to low for subsequent flows
 * - Switch our persona to Primary
 * - Set the state to Primary Run
 * - Release the flow db lock for flows to continue
 */
static void
ulp_ha_mgr_timer_cb(void *arg __rte_unused)
{
	struct tf_move_tcam_shared_entries_parms mparms = { 0 };
	struct bnxt_ulp_context *ulp_ctx;
	enum ulp_ha_mgr_state curr_state;
	struct tf *tfp;
	int32_t rc;

	ulp_ctx = bnxt_ulp_cntxt_entry_acquire();
	if (ulp_ctx == NULL) {
		BNXT_TF_DBG(INFO, "could not get the ulp context lock\n");
		ulp_ha_mgr_timer_start();
		return;
	}

	rc = ulp_ha_mgr_state_get(ulp_ctx, &curr_state);
	if (rc) {
		/*
		 * This shouldn't happen, if it does, resetart the timer
		 * and try again next time.
		 */
		BNXT_TF_DBG(ERR, "On HA CB:Failed(%d) to get state.\n", rc);
		goto cb_restart;
	}
	if (curr_state != ULP_HA_STATE_SEC_TIMER_COPY)
		goto cb_restart;

	/* Protect the flow database during the copy */
	if (bnxt_ulp_cntxt_acquire_fdb_lock(ulp_ctx)) {
		/* Should not fail, if we do, restart timer and try again */
		BNXT_TF_DBG(ERR, "Flow db lock acquire failed\n");
		goto cb_restart;
	}
	/* All paths after this point must release the fdb lock */

	/* The Primary has issued a close and we are in the timer copy
	 * phase.  Become the new Primary, Set state to Primary Run and
	 * move WC entries to Low Region.
	 */
	BNXT_TF_DBG(INFO, "On HA CB: Moving entries HI to LOW\n");
	mparms.dir = TF_DIR_RX;
	mparms.tcam_tbl_type = TF_TCAM_TBL_TYPE_WC_TCAM_HIGH;
	tfp = bnxt_ulp_cntxt_tfp_get(ulp_ctx, BNXT_ULP_SHARED_SESSION_YES);
	if (tfp == NULL) {
		BNXT_TF_DBG(ERR, "On HA CB: Unable to get the TFP.\n");
		goto unlock;
	}

	rc = tf_move_tcam_shared_entries(tfp, &mparms);
	if (rc) {
		BNXT_TF_DBG(ERR, "On HA_CB: Failed to move entries\n");
		goto unlock;
	}

	ulp_ha_mgr_region_set(ulp_ctx, ULP_HA_REGION_LOW);
	ulp_ha_mgr_app_type_set(ulp_ctx, ULP_HA_APP_TYPE_PRIM);
	ulp_ha_mgr_state_set(ulp_ctx, ULP_HA_STATE_PRIM_RUN);
	BNXT_TF_DBG(INFO, "On HA CB: SEC[SEC_TIMER_COPY] => PRIM[PRIM_RUN]\n");
unlock:
	bnxt_ulp_cntxt_release_fdb_lock(ulp_ctx);
	bnxt_ulp_cntxt_entry_release();
	return;
cb_restart:
	bnxt_ulp_cntxt_entry_release();
	ulp_ha_mgr_timer_start();
}

static int32_t
ulp_ha_mgr_timer_start(void)
{
	rte_eal_alarm_set(US_PER_S * ULP_HA_TIMER_SEC,
			  ulp_ha_mgr_timer_cb, NULL);
	return 0;
}

static void
ulp_ha_mgr_timer_cancel(struct bnxt_ulp_context *ulp_ctx)
{
	struct bnxt_ulp_ha_mgr_info *ha_info;

	ha_info = bnxt_ulp_cntxt_ptr2_ha_info_get(ulp_ctx);
	if (ha_info == NULL) {
		BNXT_TF_DBG(ERR, "Unable to get ha info\n");
		return;
	}

	ha_info->flags &= ~ULP_HA_TIMER_THREAD;
	rte_eal_alarm_cancel(ulp_ha_mgr_timer_cb, (void *)ulp_ctx);
}

int32_t
ulp_ha_mgr_init(struct bnxt_ulp_context *ulp_ctx)
{
	struct bnxt_ulp_ha_mgr_info *ha_info;
	int32_t rc;
	ha_info = rte_zmalloc("ulp_ha_mgr_info", sizeof(*ha_info), 0);
	if (!ha_info)
		return -ENOMEM;

	/* Add the HA info tbl to the ulp context. */
	bnxt_ulp_cntxt_ptr2_ha_info_set(ulp_ctx, ha_info);

	rc = pthread_mutex_init(&ha_info->ha_lock, NULL);
	if (rc) {
		PMD_DRV_LOG(ERR, "Failed to initialize ha mutex\n");
		goto cleanup;
	}

	return 0;
cleanup:
	if (ha_info != NULL)
		ulp_ha_mgr_deinit(ulp_ctx);
	return -ENOMEM;
}

void
ulp_ha_mgr_deinit(struct bnxt_ulp_context *ulp_ctx)
{
	struct bnxt_ulp_ha_mgr_info *ha_info;

	ha_info = bnxt_ulp_cntxt_ptr2_ha_info_get(ulp_ctx);
	if (ha_info == NULL) {
		BNXT_TF_DBG(ERR, "Unable to get HA Info for deinit.\n");
		return;
	}

	pthread_mutex_destroy(&ha_info->ha_lock);
	rte_free(ha_info);

	bnxt_ulp_cntxt_ptr2_ha_info_set(ulp_ctx, NULL);
}

int32_t
ulp_ha_mgr_app_type_get(struct bnxt_ulp_context *ulp_ctx,
			enum ulp_ha_mgr_app_type *app_type)
{
	struct bnxt_ulp_ha_mgr_info *ha_info;

	if (ulp_ctx == NULL || app_type == NULL) {
		BNXT_TF_DBG(ERR, "Invalid Parms.\n");
		return -EINVAL;
	}

	ha_info = bnxt_ulp_cntxt_ptr2_ha_info_get(ulp_ctx);
	if (ha_info == NULL) {
		BNXT_TF_DBG(ERR, "Unable to get the HA info.\n");
		return -EINVAL;
	}
	*app_type = ha_info->app_type;

	return 0;
}

int32_t
ulp_ha_mgr_state_get(struct bnxt_ulp_context *ulp_ctx,
		     enum ulp_ha_mgr_state *state)
{
	struct tf_get_if_tbl_entry_parms get_parms = { 0 };
	struct tf *tfp;
	uint32_t val = 0;
	int32_t rc = 0;

	if (ulp_ctx == NULL || state == NULL) {
		BNXT_TF_DBG(ERR, "Invalid parms in state get.\n");
		return -EINVAL;
	}
	tfp = bnxt_ulp_cntxt_tfp_get(ulp_ctx, BNXT_ULP_SHARED_SESSION_NO);
	if (tfp == NULL) {
		BNXT_TF_DBG(ERR, "Unable to get the TFP.\n");
		return -EINVAL;
	}

	get_parms.dir = ULP_HA_IF_TBL_DIR;
	get_parms.type = ULP_HA_IF_TBL_TYPE;
	get_parms.idx = ULP_HA_IF_TBL_IDX;
	get_parms.data = (uint8_t *)&val;
	get_parms.data_sz_in_bytes = sizeof(val);

	rc = tf_get_if_tbl_entry(tfp, &get_parms);
	if (rc)
		BNXT_TF_DBG(ERR, "Failed to read the HA state\n");

	*state = val;
	return rc;
}

int32_t
ulp_ha_mgr_open(struct bnxt_ulp_context *ulp_ctx)
{
	enum ulp_ha_mgr_state curr_state;
	int32_t rc;

	rc = ulp_ha_mgr_state_get(ulp_ctx, &curr_state);
	if (rc) {
		BNXT_TF_DBG(ERR, "Failed to get HA state on Open (%d)\n", rc);
		return -EINVAL;
	}

	/*
	 * An Open can only occur during the Init and Primary Run states. During
	 * Init, the system attempting to Open will become the only system
	 * running. During Primary Run, the system attempting to Open will
	 * become the secondary system temporarily, and should eventually be
	 * transitioned to the primary system.
	 */
	switch (curr_state) {
	case ULP_HA_STATE_INIT:
		/*
		 * No system is running, as we are the primary.  Since no other
		 * system is running, we start writing into the low region.  By
		 * writing into the low region, we save room for the secondary
		 * system to override our entries by using the high region.
		 */
		ulp_ha_mgr_app_type_set(ulp_ctx, ULP_HA_APP_TYPE_PRIM);
		ulp_ha_mgr_region_set(ulp_ctx, ULP_HA_REGION_LOW);
		rc = ulp_ha_mgr_state_set(ulp_ctx, ULP_HA_STATE_PRIM_RUN);
		if (rc) {
			BNXT_TF_DBG(ERR, "On Open: Failed to set PRIM_RUN.\n");
			return -EINVAL;
		}

		BNXT_TF_DBG(INFO, "On Open: [INIT] => PRIM[PRIM_RUN]\n");
		break;
	case ULP_HA_STATE_PRIM_RUN:
		/*
		 * The secondary system is starting in order to take over.
		 * The current primary is expected to eventually close and pass
		 * full control to this system;however, until the primary closes
		 * both are operational.
		 *
		 * The timer is started in order to determine when the
		 * primary has closed.
		 */
		ulp_ha_mgr_app_type_set(ulp_ctx, ULP_HA_APP_TYPE_SEC);
		ulp_ha_mgr_region_set(ulp_ctx, ULP_HA_REGION_HI);

		/*
		 * TODO:
		 * Clear the high region so the secondary can begin overriding
		 * the current entries.
		 */
		rc = ulp_ha_mgr_timer_start();
		if (rc) {
			BNXT_TF_DBG(ERR, "Unable to start timer on HA Open.\n");
			return -EINVAL;
		}

		rc = ulp_ha_mgr_state_set(ulp_ctx, ULP_HA_STATE_PRIM_SEC_RUN);
		if (rc) {
			BNXT_TF_DBG(ERR, "On Open: Failed to set PRIM_SEC_RUN\n");
			return -EINVAL;
		}
		BNXT_TF_DBG(INFO, "On Open: [PRIM_RUN] => [PRIM_SEC_RUN]\n");
		break;
	default:
		BNXT_TF_DBG(ERR, "On Open: Unknown state 0x%x\n", curr_state);
		return -EINVAL;
	}

	return 0;
}

int32_t
ulp_ha_mgr_close(struct bnxt_ulp_context *ulp_ctx)
{
	enum ulp_ha_mgr_state curr_state, next_state, poll_state;
	enum ulp_ha_mgr_app_type app_type;
	int32_t timeout;
	int32_t rc;

	rc = ulp_ha_mgr_state_get(ulp_ctx, &curr_state);
	if (rc) {
		BNXT_TF_DBG(ERR, "On Close: Failed(%d) to get HA state\n", rc);
		return -EINVAL;
	}

	rc = ulp_ha_mgr_app_type_get(ulp_ctx, &app_type);
	if (rc) {
		BNXT_TF_DBG(ERR, "On Close: Failed to get the app type.\n");
		return -EINVAL;
	}

	if (curr_state == ULP_HA_STATE_PRIM_RUN &&
	    app_type == ULP_HA_APP_TYPE_PRIM) {
		/*
		 * Only the primary is running, so a close effectively moves the
		 * system back to INIT.
		 */
		next_state = ULP_HA_STATE_INIT;
		ulp_ha_mgr_state_set(ulp_ctx, next_state);
		BNXT_TF_DBG(INFO, "On Close: PRIM[PRIM_RUN] => [INIT]\n");
	} else if (curr_state == ULP_HA_STATE_PRIM_SEC_RUN &&
		  app_type == ULP_HA_APP_TYPE_PRIM) {
		/*
		 * While both are running, the primary received a close.
		 * Cleanup the flows, set the COPY state, and wait for the
		 * secondary to become the Primary.
		 */
		BNXT_TF_DBG(INFO,
			    "On Close: PRIM[PRIM_SEC_RUN] flushing flows.\n");

		ulp_flow_db_flush_flows(ulp_ctx, BNXT_ULP_FDB_TYPE_REGULAR);
		ulp_ha_mgr_state_set(ulp_ctx, ULP_HA_STATE_SEC_TIMER_COPY);

		/*
		 * TODO: This needs to be bounded in case the other system does
		 * not move to PRIM_RUN.
		 */
		BNXT_TF_DBG(INFO,
			    "On Close: PRIM[PRIM_SEC_RUN] => [Copy], enter wait.\n");
		timeout = ULP_HA_WAIT_TIMEOUT;
		do {
			rte_delay_ms(ULP_HA_WAIT_TIME);
			rc = ulp_ha_mgr_state_get(ulp_ctx, &poll_state);
			if (rc) {
				BNXT_TF_DBG(ERR,
					    "Failed to get HA state on Close (%d)\n",
					    rc);
				goto cleanup;
			}
			timeout -= ULP_HA_WAIT_TIME;
			BNXT_TF_DBG(INFO,
				    "On Close: Waiting %d ms for PRIM_RUN\n",
				    timeout);
		} while (poll_state != ULP_HA_STATE_PRIM_RUN && timeout > 0);

		if (timeout <= 0) {
			BNXT_TF_DBG(ERR, "On Close: SEC[COPY] Timed out\n");
			goto cleanup;
		}

		BNXT_TF_DBG(INFO, "On Close: PRIM[PRIM_SEC_RUN] => [COPY]\n");
	} else if (curr_state == ULP_HA_STATE_PRIM_SEC_RUN &&
		   app_type == ULP_HA_APP_TYPE_SEC) {
		/*
		 * While both are running, the secondary unexpectedly received a
		 * close.  Cancel the timer, set the state to Primary RUN since
		 * it is the only one running.
		 */
		ulp_ha_mgr_timer_cancel(ulp_ctx);
		ulp_ha_mgr_state_set(ulp_ctx, ULP_HA_STATE_PRIM_RUN);

		BNXT_TF_DBG(INFO, "On Close: SEC[PRIM_SEC_RUN] => [PRIM_RUN]\n");
	} else if (curr_state == ULP_HA_STATE_SEC_TIMER_COPY &&
		   app_type == ULP_HA_APP_TYPE_SEC) {
		/*
		 * While both were running and the Secondary went into copy,
		 * secondary received a close.  Wait until the former Primary
		 * clears the copy stage, close, and set to INIT.
		 */
		BNXT_TF_DBG(INFO, "On Close: SEC[COPY] wait for PRIM_RUN\n");

		timeout = ULP_HA_WAIT_TIMEOUT;
		do {
			rte_delay_ms(ULP_HA_WAIT_TIME);
			rc = ulp_ha_mgr_state_get(ulp_ctx, &poll_state);
			if (rc) {
				BNXT_TF_DBG(ERR,
					    "Failed to get HA state on Close (%d)\n",
					    rc);
				goto cleanup;
			}

			timeout -= ULP_HA_WAIT_TIME;
			BNXT_TF_DBG(INFO,
				    "On Close: Waiting %d ms for PRIM_RUN\n",
				    timeout);
		} while (poll_state != ULP_HA_STATE_PRIM_RUN &&
			 timeout >= 0);

		if (timeout <= 0) {
			BNXT_TF_DBG(ERR,
				    "On Close: SEC[COPY] Timed out\n");
			goto cleanup;
		}

		next_state = ULP_HA_STATE_INIT;
		rc = ulp_ha_mgr_state_set(ulp_ctx, next_state);
		if (rc) {
			BNXT_TF_DBG(ERR,
				    "On Close: Failed to set state to INIT(%x)\n",
				    rc);
			goto cleanup;
		}

		BNXT_TF_DBG(INFO,
			    "On Close: SEC[COPY] => [INIT] after %d ms\n",
			    ULP_HA_WAIT_TIMEOUT - timeout);
	} else {
		BNXT_TF_DBG(ERR, "On Close: Invalid type/state %d/%d\n",
			    curr_state, app_type);
	}
cleanup:
	return rc;
}

int32_t
ulp_ha_mgr_region_get(struct bnxt_ulp_context *ulp_ctx,
		      enum ulp_ha_mgr_region *region)
{
	struct bnxt_ulp_ha_mgr_info *ha_info;

	if (ulp_ctx == NULL || region == NULL) {
		BNXT_TF_DBG(ERR, "Invalid params in ha region get.\n");
		return -EINVAL;
	}

	ha_info = bnxt_ulp_cntxt_ptr2_ha_info_get(ulp_ctx);
	if (ha_info == NULL) {
		BNXT_TF_DBG(ERR, "Unable to get ha info\n");
		return -EINVAL;
	}
	*region = ha_info->region;

	return 0;
}