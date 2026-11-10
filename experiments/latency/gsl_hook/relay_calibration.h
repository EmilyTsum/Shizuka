/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIZUKA_RELAY_CALIBRATION_H
#define SHIZUKA_RELAY_CALIBRATION_H
#include "relay_transaction.h"

struct shizuka_gsl_cal_call {
    uint32_t caller_rva,opcode,command_size,response_size;
    int32_t original_status;
    const uint32_t *sgids;
    uint32_t num_sgids;
    /* GKV is checked from the graph owner's epoch record, not from the CKV
     * fields in AcdbSgIdCalKeyVector: the command does not contain a GKV.
     */
    const struct shizuka_gsl_key *current_gkv;
    uint32_t num_gkv;
    const struct shizuka_gsl_key *prior_ckv,*new_ckv;
    uint32_t num_prior_ckv,num_new_ckv;
    uint8_t *data;
    uint32_t data_size;
    size_t capacity;
};
struct shizuka_relay_cal_transaction {
    struct shizuka_relay_scope scope;
    uint32_t pending,capacity,num_sgids,num_prior_ckv,num_new_ckv;
    uint32_t sgids[SHIZUKA_GSL_MAX_SGIDS];
    struct shizuka_gsl_key prior_ckv[SHIZUKA_GSL_MAX_KEYS],new_ckv[SHIZUKA_GSL_MAX_KEYS];
};
struct shizuka_relay_cal_result { uint32_t actual_size,removed; };
#define SHIZUKA_RELAY_CAL_EMPTY 3

/* Called after original acdb_ioctl exactly once. Query keeps its original
 * size/capacity and captures thread/epoch/CKVs. Fill validates the entire APM
 * blob before compacting only (43f7|43fa, 08001026|08001031|08001313) records.
 * All remaining records and padding stay byte-identical and ordered.
 * COMMITTED: write actual_size to packed AcdbBlob.buf_size at offset0.
 * EMPTY: return AR_ENOTEXIST(19) to let the original GSL calibration caller
 * skip an empty SET_CFG. ABORT: fail this owned calibration; do not send data.
 * There is no graph/GPR operation, allocation, or unscoped filtering API here.
 */
int shizuka_relay_after_calibration(const struct shizuka_relay_transaction *owner,
    struct shizuka_relay_cal_transaction *transaction,const struct shizuka_relay_scope *scope,
    const struct shizuka_gsl_cal_call *call,struct shizuka_relay_cal_result *result);
void shizuka_relay_calibration_reset(struct shizuka_relay_cal_transaction *);
#endif
