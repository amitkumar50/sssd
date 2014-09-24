/*
    SSSD

    IPA Identity Backend Module for views and overrides

    Authors:
        Sumit Bose <sbose@redhat.com>

    Copyright (C) 2014 Red Hat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "util/util.h"
#include "util/strtonum.h"
#include "providers/ldap/sdap_async.h"
#include "providers/ipa/ipa_id.h"

static errno_t be_acct_req_to_override_filter(TALLOC_CTX *mem_ctx,
                                              struct ipa_options *ipa_opts,
                                              struct be_acct_req *ar,
                                              char **override_filter)
{
    char *filter;
    uint32_t id;
    char *endptr;

    switch (ar->filter_type) {
    case BE_FILTER_NAME:
        switch ((ar->entry_type & BE_REQ_TYPE_MASK)) {
        case BE_REQ_USER:
        case BE_REQ_INITGROUPS:
            filter = talloc_asprintf(mem_ctx, "(&(objectClass=%s)(%s=%s))",
                         ipa_opts->override_map[IPA_OC_OVERRIDE_USER].name,
                         ipa_opts->override_map[IPA_AT_OVERRIDE_USER_NAME].name,
                         ar->filter_value);
            break;

         case BE_REQ_GROUP:
            filter = talloc_asprintf(mem_ctx, "(&(objectClass=%s)(%s=%s))",
                        ipa_opts->override_map[IPA_OC_OVERRIDE_GROUP].name,
                        ipa_opts->override_map[IPA_AT_OVERRIDE_GROUP_NAME].name,
                        ar->filter_value);
            break;

         case BE_REQ_USER_AND_GROUP:
            filter = talloc_asprintf(mem_ctx, "(&(objectClass=%s)(|(%s=%s)(%s=%s)))",
                        ipa_opts->override_map[IPA_OC_OVERRIDE].name,
                        ipa_opts->override_map[IPA_AT_OVERRIDE_USER_NAME].name,
                        ar->filter_value,
                        ipa_opts->override_map[IPA_AT_OVERRIDE_GROUP_NAME].name,
                        ar->filter_value);
            break;
        default:
            DEBUG(SSSDBG_CRIT_FAILURE, "Unexpected entry type [%d] for name filter.\n",
                                       ar->entry_type);
            return EINVAL;
        }
        break;

    case BE_FILTER_IDNUM:
        errno = 0;
        id = strtouint32(ar->filter_value, &endptr, 10);
        if (errno != 0|| *endptr != '\0' || (ar->filter_value == endptr)) {
            DEBUG(SSSDBG_CRIT_FAILURE, "Invalid id value [%s].\n",
                                       ar->filter_value);
            return EINVAL;
        }
        switch ((ar->entry_type & BE_REQ_TYPE_MASK)) {
        case BE_REQ_USER:
        case BE_REQ_INITGROUPS:
            filter = talloc_asprintf(mem_ctx, "(&(objectClass=%s)(%s=%"PRIu32"))",
                        ipa_opts->override_map[IPA_OC_OVERRIDE_USER].name,
                        ipa_opts->override_map[IPA_AT_OVERRIDE_UID_NUMBER].name,
                        id);
            break;

         case BE_REQ_GROUP:
            filter = talloc_asprintf(mem_ctx,
                  "(&(objectClass=%s)(%s=%"PRIu32"))",
                  ipa_opts->override_map[IPA_OC_OVERRIDE_GROUP].name,
                  ipa_opts->override_map[IPA_AT_OVERRIDE_GROUP_GID_NUMBER].name,
                  id);
            break;

         case BE_REQ_USER_AND_GROUP:
            filter = talloc_asprintf(mem_ctx,
                  "(&(objectClass=%s)(|(%s=%"PRIu32")(%s=%"PRIu32")))",
                  ipa_opts->override_map[IPA_OC_OVERRIDE].name,
                  ipa_opts->override_map[IPA_AT_OVERRIDE_UID_NUMBER].name,
                  id,
                  ipa_opts->override_map[IPA_AT_OVERRIDE_GROUP_GID_NUMBER].name,
                  id);
            break;
        default:
            DEBUG(SSSDBG_CRIT_FAILURE,
                  "Unexpected entry type [%d] for id filter.\n",
                  ar->entry_type);
            return EINVAL;
        }
        break;

    case BE_FILTER_SECID:
        if ((ar->entry_type & BE_REQ_TYPE_MASK) == BE_REQ_BY_SECID) {
            filter = talloc_asprintf(mem_ctx, "(&(objectClass=%s)(%s=:SID:%s))",
                       ipa_opts->override_map[IPA_OC_OVERRIDE].name,
                       ipa_opts->override_map[IPA_AT_OVERRIDE_ANCHOR_UUID].name,
                       ar->filter_value);
        } else {
            DEBUG(SSSDBG_CRIT_FAILURE,
                  "Unexpected entry type [%d] for SID filter.\n",
                  ar->entry_type);
            return EINVAL;
        }
        break;

    default:
        DEBUG(SSSDBG_OP_FAILURE, "Invalid sub-domain filter type.\n");
        return EINVAL;
    }

    if (filter == NULL) {
        DEBUG(SSSDBG_OP_FAILURE, "talloc_asprintf failed.\n");
        return ENOMEM;
    }

    *override_filter = filter;

    return EOK;
}

errno_t get_be_acct_req_for_sid(TALLOC_CTX *mem_ctx, const char *sid,
                                const char *domain_name,
                                struct be_acct_req **_ar)
{
    struct be_acct_req *ar;

    ar = talloc_zero(mem_ctx, struct be_acct_req);
    if (ar == NULL) {
        DEBUG(SSSDBG_OP_FAILURE, "talloc_zero failed.\n");
        return ENOMEM;
    }

    ar->entry_type = BE_REQ_BY_SECID;
    ar->filter_type = BE_FILTER_SECID;
    ar->filter_value = talloc_strdup(ar, sid);
    ar->domain = talloc_strdup(ar, domain_name);
    if (ar->filter_value == NULL || ar->domain == NULL) {
        DEBUG(SSSDBG_OP_FAILURE, "talloc_strdup failed.\n");
        talloc_free(ar);
        return ENOMEM;
    }


    *_ar = ar;

    return EOK;
}

struct ipa_get_ad_override_state {
    struct tevent_context *ev;
    struct sdap_id_ctx *sdap_id_ctx;
    struct ipa_options *ipa_options;
    const char *ipa_realm;
    const char *ipa_view_name;
    struct be_acct_req *ar;

    struct sdap_id_op *sdap_op;
    int dp_error;
    struct sysdb_attrs *override_attrs;
    char *filter;
};

static void ipa_get_ad_override_connect_done(struct tevent_req *subreq);
static void ipa_get_ad_override_done(struct tevent_req *subreq);

struct tevent_req *ipa_get_ad_override_send(TALLOC_CTX *mem_ctx,
                                            struct tevent_context *ev,
                                            struct sdap_id_ctx *sdap_id_ctx,
                                            struct ipa_options *ipa_options,
                                            const char *ipa_realm,
                                            const char *view_name,
                                            struct be_acct_req *ar)
{
    int ret;
    struct tevent_req *req;
    struct tevent_req *subreq;
    struct ipa_get_ad_override_state *state;

    req = tevent_req_create(mem_ctx, &state, struct ipa_get_ad_override_state);
    if (req == NULL) {
        DEBUG(SSSDBG_OP_FAILURE, "tevent_req_create failed.\n");
        return NULL;
    }

    state->ev = ev;
    state->sdap_id_ctx = sdap_id_ctx;
    state->ipa_options = ipa_options;
    state->ipa_realm = ipa_realm;
    if (strcmp(view_name, SYSDB_DEFAULT_VIEW_NAME) == 0) {
        state->ipa_view_name = IPA_DEFAULT_VIEW_NAME;
    } else {
        state->ipa_view_name = view_name;
    }
    state->ar = ar;
    state->dp_error = -1;
    state->override_attrs = NULL;
    state->filter = NULL;

    state->sdap_op = sdap_id_op_create(state,
                                       state->sdap_id_ctx->conn->conn_cache);
    if (state->sdap_op == NULL) {
        DEBUG(SSSDBG_OP_FAILURE, "sdap_id_op_create failed\n");
        ret = ENOMEM;
        goto done;
    }

    subreq = sdap_id_op_connect_send(state->sdap_op, state, &ret);
    if (subreq == NULL) {
        DEBUG(SSSDBG_OP_FAILURE, "sdap_id_op_connect_send failed: %d(%s).\n",
                                  ret, strerror(ret));
        goto done;
    }

    tevent_req_set_callback(subreq, ipa_get_ad_override_connect_done, req);

    return req;

done:
    if (ret != EOK) {
        state->dp_error = DP_ERR_FATAL;
        tevent_req_error(req, ret);
    } else {
        state->dp_error = DP_ERR_OK;
        tevent_req_done(req);
    }
    tevent_req_post(req, state->ev);

    return req;
}

static void ipa_get_ad_override_connect_done(struct tevent_req *subreq)
{
    struct tevent_req *req = tevent_req_callback_data(subreq,
                                                      struct tevent_req);
    struct ipa_get_ad_override_state *state = tevent_req_data(req,
                                              struct ipa_get_ad_override_state);
    int ret;
    char *basedn;
    char *search_base;
    struct ipa_options *ipa_opts = state->ipa_options;

    ret = sdap_id_op_connect_recv(subreq, &state->dp_error);
    talloc_zfree(subreq);
    if (ret != EOK) {
        if (state->dp_error == DP_ERR_OFFLINE) {
            DEBUG(SSSDBG_MINOR_FAILURE,
                  "No IPA server is available, going offline\n");
        } else {
            DEBUG(SSSDBG_OP_FAILURE,
                  "Failed to connect to IPA server: [%d](%s)\n",
                   ret, strerror(ret));
        }

        goto fail;
    }

    ret = domain_to_basedn(state, state->ipa_realm, &basedn);
    if (ret != EOK) {
        DEBUG(SSSDBG_OP_FAILURE, "domain_to_basedn failed.\n");
        goto fail;
    }

    search_base = talloc_asprintf(state, "cn=%s,%s", state->ipa_view_name,
                                       ipa_opts->views_search_bases[0]->basedn);
    if (search_base == NULL) {
        DEBUG(SSSDBG_OP_FAILURE, "talloc_asprintf failed.\n");
        ret = ENOMEM;
        goto fail;
    }

    ret = be_acct_req_to_override_filter(state, state->ipa_options, state->ar,
                                         &state->filter);
    if (ret != EOK) {
        DEBUG(SSSDBG_OP_FAILURE, "be_acct_req_to_override_filter failed.\n");
        goto fail;
    }

    DEBUG(SSSDBG_TRACE_ALL,
          "Searching for overrides in view [%s] with filter [%s].\n",
          state->ipa_view_name, state->filter);

    subreq = sdap_get_generic_send(state, state->ev, state->sdap_id_ctx->opts,
                                 sdap_id_op_handle(state->sdap_op), search_base,
                                 LDAP_SCOPE_SUBTREE,
                                 state->filter, NULL,
                                 state->ipa_options->override_map,
                                 IPA_OPTS_OVERRIDE,
                                 dp_opt_get_int(state->sdap_id_ctx->opts->basic,
                                                SDAP_ENUM_SEARCH_TIMEOUT),
                                 false);
    if (subreq == NULL) {
        DEBUG(SSSDBG_OP_FAILURE, "sdap_get_generic_send failed.\n");
        ret = ENOMEM;
        goto fail;
    }

    tevent_req_set_callback(subreq, ipa_get_ad_override_done, req);
    return;

fail:
    state->dp_error = DP_ERR_FATAL;
    tevent_req_error(req, ret);
    return;
}

static void ipa_get_ad_override_done(struct tevent_req *subreq)
{
    struct tevent_req *req = tevent_req_callback_data(subreq,
                                                      struct tevent_req);
    struct ipa_get_ad_override_state *state = tevent_req_data(req,
                                              struct ipa_get_ad_override_state);
    int ret;
    size_t reply_count = 0;
    struct sysdb_attrs **reply = NULL;

    ret = sdap_get_generic_recv(subreq, state, &reply_count, &reply);
    talloc_zfree(subreq);
    if (ret != EOK) {
        DEBUG(SSSDBG_OP_FAILURE, "ipa_get_ad_override request failed.\n");
        goto fail;
    }

    if (reply_count == 0) {
        DEBUG(SSSDBG_TRACE_ALL, "No override found with filter [%s].\n",
                                state->filter);
        state->dp_error = DP_ERR_OK;
        tevent_req_done(req);
        return;
    } else if (reply_count > 1) {
        DEBUG(SSSDBG_CRIT_FAILURE,
              "Found [%zu] overrides with filter [%s], expected only 1.\n",
              reply_count, state->filter);
        ret = EINVAL;
        goto fail;
    }

    DEBUG(SSSDBG_TRACE_ALL, "Found override for object with filter [%s].\n",
                            state->filter);

    state->override_attrs = reply[0];
    state->dp_error = DP_ERR_OK;
    tevent_req_done(req);
    return;

fail:
    state->dp_error = DP_ERR_FATAL;
    tevent_req_error(req, ret);
    return;
}

errno_t ipa_get_ad_override_recv(struct tevent_req *req, int *dp_error_out,
                                 TALLOC_CTX *mem_ctx,
                                 struct sysdb_attrs **override_attrs)
{
    struct ipa_get_ad_override_state *state = tevent_req_data(req,
                                              struct ipa_get_ad_override_state);

    if (dp_error_out != NULL) {
        *dp_error_out = state->dp_error;
    }

    TEVENT_REQ_RETURN_ON_ERROR(req);

    if (override_attrs != NULL) {
        *override_attrs = talloc_steal(mem_ctx, state->override_attrs);
    }

    return EOK;
}