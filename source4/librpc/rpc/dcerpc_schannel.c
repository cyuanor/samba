/* 
   Unix SMB/CIFS implementation.

   dcerpc schannel operations

   Copyright (C) Andrew Tridgell 2004
   Copyright (C) Andrew Bartlett <abartlet@samba.org> 2004-2005
   Copyright (C) Rafal Szczesniak 2006

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

#include "includes.h"
#include <tevent.h>
#include "auth/auth.h"
#include "libcli/composite/composite.h"
#include "libcli/auth/libcli_auth.h"
#include "librpc/gen_ndr/ndr_netlogon.h"
#include "librpc/gen_ndr/ndr_netlogon_c.h"
#include "auth/credentials/credentials.h"
#include "librpc/rpc/dcerpc_proto.h"
#include "param/param.h"
#include "lib/param/loadparm.h"

struct schannel_key_state {
	struct dcerpc_pipe *pipe;
	struct dcerpc_pipe *pipe2;
	struct dcerpc_binding *binding;
	bool dcerpc_schannel_auto;
	struct cli_credentials *credentials;
	struct netlogon_creds_CredentialState *creds;
	uint32_t requested_negotiate_flags;
	uint32_t required_negotiate_flags;
	uint32_t local_negotiate_flags;
	uint32_t remote_negotiate_flags;
	struct netr_Credential credentials1;
	struct netr_Credential credentials2;
	struct netr_Credential credentials3;
	struct netr_ServerReqChallenge r;
	struct netr_ServerAuthenticate2 a;
	const struct samr_Password *mach_pwd;
};


static void continue_secondary_connection(struct composite_context *ctx);
static void continue_bind_auth_none(struct composite_context *ctx);
static void continue_srv_challenge(struct tevent_req *subreq);
static void continue_srv_auth2(struct tevent_req *subreq);
static void continue_get_negotiated_capabilities(struct tevent_req *subreq);
static void continue_get_client_capabilities(struct tevent_req *subreq);


/*
  Stage 2 of schannel_key: Receive endpoint mapping and request secondary
  rpc connection
*/
static void continue_epm_map_binding(struct composite_context *ctx)
{
	struct composite_context *c;
	struct schannel_key_state *s;
	struct composite_context *sec_conn_req;

	c = talloc_get_type(ctx->async.private_data, struct composite_context);
	s = talloc_get_type(c->private_data, struct schannel_key_state);

	/* receive endpoint mapping */
	c->status = dcerpc_epm_map_binding_recv(ctx);
	if (!NT_STATUS_IS_OK(c->status)) {
		DEBUG(0,("Failed to map DCERPC/TCP NCACN_NP pipe for '%s' - %s\n",
			 NDR_NETLOGON_UUID, nt_errstr(c->status)));
		composite_error(c, c->status);
		return;
	}

	/* send a request for secondary rpc connection */
	sec_conn_req = dcerpc_secondary_connection_send(s->pipe,
							s->binding);
	if (composite_nomem(sec_conn_req, c)) return;

	composite_continue(c, sec_conn_req, continue_secondary_connection, c);
}


/*
  Stage 3 of schannel_key: Receive secondary rpc connection and perform
  non-authenticated bind request
*/
static void continue_secondary_connection(struct composite_context *ctx)
{
	struct composite_context *c;
	struct schannel_key_state *s;
	struct composite_context *auth_none_req;

	c = talloc_get_type(ctx->async.private_data, struct composite_context);
	s = talloc_get_type(c->private_data, struct schannel_key_state);

	/* receive secondary rpc connection */
	c->status = dcerpc_secondary_connection_recv(ctx, &s->pipe2);
	if (!composite_is_ok(c)) return;

	talloc_steal(s, s->pipe2);

	/* initiate a non-authenticated bind */
	auth_none_req = dcerpc_bind_auth_none_send(c, s->pipe2, &ndr_table_netlogon);
	if (composite_nomem(auth_none_req, c)) return;

	composite_continue(c, auth_none_req, continue_bind_auth_none, c);
}


/*
  Stage 4 of schannel_key: Receive non-authenticated bind and get
  a netlogon challenge
*/
static void continue_bind_auth_none(struct composite_context *ctx)
{
	struct composite_context *c;
	struct schannel_key_state *s;
	struct tevent_req *subreq;

	c = talloc_get_type(ctx->async.private_data, struct composite_context);
	s = talloc_get_type(c->private_data, struct schannel_key_state);

	/* receive result of non-authenticated bind request */
	c->status = dcerpc_bind_auth_none_recv(ctx);
	if (!composite_is_ok(c)) return;
	
	/* prepare a challenge request */
	s->r.in.server_name   = talloc_asprintf(c, "\\\\%s", dcerpc_server_name(s->pipe));
	if (composite_nomem(s->r.in.server_name, c)) return;
	s->r.in.computer_name = cli_credentials_get_workstation(s->credentials);
	s->r.in.credentials   = &s->credentials1;
	s->r.out.return_credentials  = &s->credentials2;
	
	generate_random_buffer(s->credentials1.data, sizeof(s->credentials1.data));

	/*
	  request a netlogon challenge - a rpc request over opened secondary pipe
	*/
	subreq = dcerpc_netr_ServerReqChallenge_r_send(s, c->event_ctx,
						       s->pipe2->binding_handle,
						       &s->r);
	if (composite_nomem(subreq, c)) return;

	tevent_req_set_callback(subreq, continue_srv_challenge, c);
}


/*
  Stage 5 of schannel_key: Receive a challenge and perform authentication
  on the netlogon pipe
*/
static void continue_srv_challenge(struct tevent_req *subreq)
{
	struct composite_context *c;
	struct schannel_key_state *s;

	c = tevent_req_callback_data(subreq, struct composite_context);
	s = talloc_get_type(c->private_data, struct schannel_key_state);

	/* receive rpc request result - netlogon challenge */
	c->status = dcerpc_netr_ServerReqChallenge_r_recv(subreq, s);
	TALLOC_FREE(subreq);
	if (!composite_is_ok(c)) return;

	/* prepare credentials for auth2 request */
	s->mach_pwd = cli_credentials_get_nt_hash(s->credentials, c);
	if (s->mach_pwd == NULL) {
		return composite_error(c, NT_STATUS_INTERNAL_ERROR);
	}

	/* auth2 request arguments */
	s->a.in.server_name      = s->r.in.server_name;
	s->a.in.account_name     = cli_credentials_get_username(s->credentials);
	s->a.in.secure_channel_type =
		cli_credentials_get_secure_channel_type(s->credentials);
	s->a.in.computer_name    = cli_credentials_get_workstation(s->credentials);
	s->a.in.negotiate_flags  = &s->requested_negotiate_flags;
	s->a.in.credentials      = &s->credentials3;
	s->a.out.negotiate_flags = &s->remote_negotiate_flags;
	s->a.out.return_credentials     = &s->credentials3;

	s->creds = netlogon_creds_client_init(s, 
					      s->a.in.account_name, 
					      s->a.in.computer_name,
					      s->a.in.secure_channel_type,
					      &s->credentials1, &s->credentials2,
					      s->mach_pwd, &s->credentials3,
					      s->requested_negotiate_flags,
					      s->local_negotiate_flags);
	if (composite_nomem(s->creds, c)) {
		return;
	}
	/*
	  authenticate on the netlogon pipe - a rpc request over secondary pipe
	*/
	subreq = dcerpc_netr_ServerAuthenticate2_r_send(s, c->event_ctx,
							s->pipe2->binding_handle,
							&s->a);
	if (composite_nomem(subreq, c)) return;

	tevent_req_set_callback(subreq, continue_srv_auth2, c);
}


/*
  Stage 6 of schannel_key: Receive authentication request result and verify
  received credentials
*/
static void continue_srv_auth2(struct tevent_req *subreq)
{
	struct composite_context *c;
	struct schannel_key_state *s;
	enum dcerpc_AuthType auth_type;
	enum dcerpc_AuthLevel auth_level;
	NTSTATUS status;

	c = tevent_req_callback_data(subreq, struct composite_context);
	s = talloc_get_type(c->private_data, struct schannel_key_state);

	dcerpc_binding_handle_auth_info(s->pipe2->binding_handle,
					&auth_type,
					&auth_level);

	/* receive rpc request result - auth2 credentials */ 
	c->status = dcerpc_netr_ServerAuthenticate2_r_recv(subreq, s);
	TALLOC_FREE(subreq);
	if (!composite_is_ok(c)) return;

	if (!NT_STATUS_EQUAL(s->a.out.result, NT_STATUS_ACCESS_DENIED) &&
	    !NT_STATUS_IS_OK(s->a.out.result)) {
		composite_error(c, s->a.out.result);
		return;
	}

	{
		uint32_t rqf = s->required_negotiate_flags;
		uint32_t rf = s->remote_negotiate_flags;
		uint32_t lf = s->local_negotiate_flags;

		if ((rf & NETLOGON_NEG_SUPPORTS_AES) &&
		    (lf & NETLOGON_NEG_SUPPORTS_AES))
		{
			rqf &= ~NETLOGON_NEG_ARCFOUR;
			rqf &= ~NETLOGON_NEG_STRONG_KEYS;
		}

		if ((rqf & rf) != rqf) {
			rqf = s->required_negotiate_flags;
			DBG_ERR("The client capabilities don't match "
				"the server capabilities: local[0x%08X] "
				"required[0x%08X] remote[0x%08X]\n",
				lf, rqf, rf);
			composite_error(c, NT_STATUS_DOWNGRADE_DETECTED);
			return;
		}
	}

	/*
	 * Strong keys could be unsupported (NT4) or disabled. So retry with the
	 * flags returned by the server. - asn
	 */
	if (NT_STATUS_EQUAL(s->a.out.result, NT_STATUS_ACCESS_DENIED)) {
		uint32_t lf = s->local_negotiate_flags;
		const char *ln = NULL;
		uint32_t rf = s->remote_negotiate_flags;
		const char *rn = NULL;

		if ((lf & rf) == lf) {
			/*
			 * without a change in flags
			 * there's no need to retry...
			 */
			s->dcerpc_schannel_auto = false;
		}

		if (!s->dcerpc_schannel_auto) {
			composite_error(c, s->a.out.result);
			return;
		}
		s->dcerpc_schannel_auto = false;

		if (lf & NETLOGON_NEG_SUPPORTS_AES)  {
			ln = "aes";
			if (rf & NETLOGON_NEG_SUPPORTS_AES) {
				composite_error(c, s->a.out.result);
				return;
			}
		} else if (lf & NETLOGON_NEG_STRONG_KEYS) {
			ln = "strong";
			if (rf & NETLOGON_NEG_STRONG_KEYS) {
				composite_error(c, s->a.out.result);
				return;
			}
		} else {
			ln = "des";
		}

		if (rf & NETLOGON_NEG_SUPPORTS_AES)  {
			rn = "aes";
		} else if (rf & NETLOGON_NEG_STRONG_KEYS) {
			rn = "strong";
		} else {
			rn = "des";
		}

		DEBUG(3, ("Server doesn't support %s keys, downgrade to %s"
			  "and retry! local[0x%08X] remote[0x%08X]\n",
			  ln, rn, lf, rf));

		s->local_negotiate_flags &= s->remote_negotiate_flags;

		generate_random_buffer(s->credentials1.data,
				       sizeof(s->credentials1.data));

		subreq = dcerpc_netr_ServerReqChallenge_r_send(s,
							       c->event_ctx,
							       s->pipe2->binding_handle,
							       &s->r);
		if (composite_nomem(subreq, c)) return;

		tevent_req_set_callback(subreq, continue_srv_challenge, c);
		return;
	}

	/* verify credentials */
	status = netlogon_creds_client_verify(s->creds,
					      s->a.out.return_credentials,
					      auth_type,
					      auth_level);
	if (!NT_STATUS_IS_OK(status)) {
		composite_error(c, status);
		return;
	}

	if (s->requested_negotiate_flags == s->local_negotiate_flags) {
		/*
		 * Without a downgrade in the crypto we proposed
		 * we can adjust the otherwise downgraded flags
		 * before storing.
		 */
		s->creds->negotiate_flags &= s->remote_negotiate_flags;
	} else if (s->local_negotiate_flags != s->remote_negotiate_flags) {
		/*
		 * We downgraded our crypto once, we should not
		 * allow any additional downgrade!
		 */
		DBG_ERR("%s: NT_STATUS_DOWNGRADE_DETECTED\n", __location__);
		composite_error(c, NT_STATUS_DOWNGRADE_DETECTED);
		return;
	}

	composite_done(c);
}

/*
  Initiate establishing a schannel key using netlogon challenge
  on a secondary pipe
*/
static struct composite_context *dcerpc_schannel_key_send(TALLOC_CTX *mem_ctx,
						   struct dcerpc_pipe *p,
						   struct cli_credentials *credentials,
						   struct loadparm_context *lp_ctx)
{
	struct composite_context *c;
	struct schannel_key_state *s;
	struct composite_context *epm_map_req;
	enum netr_SchannelType schannel_type = cli_credentials_get_secure_channel_type(credentials);
	struct cli_credentials *epm_creds = NULL;
	bool reject_md5_servers = false;
	bool require_strong_key = false;

	/* composite context allocation and setup */
	c = composite_create(mem_ctx, p->conn->event_ctx);
	if (c == NULL) return NULL;

	s = talloc_zero(c, struct schannel_key_state);
	if (composite_nomem(s, c)) return c;
	c->private_data = s;

	/* store parameters in the state structure */
	s->pipe        = p;
	s->credentials = credentials;
	s->local_negotiate_flags = NETLOGON_NEG_AUTH2_FLAGS;
	s->required_negotiate_flags = NETLOGON_NEG_AUTHENTICATED_RPC;

	/* allocate credentials */
	if (s->pipe->conn->flags & DCERPC_SCHANNEL_128) {
		s->local_negotiate_flags = NETLOGON_NEG_AUTH2_ADS_FLAGS;
		require_strong_key = true;
	}
	if (s->pipe->conn->flags & DCERPC_SCHANNEL_AES) {
		s->local_negotiate_flags = NETLOGON_NEG_AUTH2_ADS_FLAGS;
		reject_md5_servers = true;
	}
	if (s->pipe->conn->flags & DCERPC_SCHANNEL_AUTO) {
		s->local_negotiate_flags = NETLOGON_NEG_AUTH2_ADS_FLAGS;
		s->local_negotiate_flags |= NETLOGON_NEG_SUPPORTS_AES;
		s->dcerpc_schannel_auto = true;
		reject_md5_servers = lpcfg_reject_md5_servers(lp_ctx);
		require_strong_key = lpcfg_require_strong_key(lp_ctx);
	}

	if (lpcfg_weak_crypto(lp_ctx) == SAMBA_WEAK_CRYPTO_DISALLOWED) {
		reject_md5_servers = true;
	}

	if (reject_md5_servers) {
		require_strong_key = true;
	}

	if (require_strong_key) {
		s->required_negotiate_flags |= NETLOGON_NEG_ARCFOUR;
		s->required_negotiate_flags |= NETLOGON_NEG_STRONG_KEYS;
	}

	if (reject_md5_servers) {
		s->required_negotiate_flags |= NETLOGON_NEG_PASSWORD_SET2;
		s->required_negotiate_flags |= NETLOGON_NEG_SUPPORTS_AES;
	}

	s->local_negotiate_flags |= s->required_negotiate_flags;

	if (s->required_negotiate_flags & NETLOGON_NEG_SUPPORTS_AES) {
		s->required_negotiate_flags &= ~NETLOGON_NEG_ARCFOUR;
		s->required_negotiate_flags &= ~NETLOGON_NEG_STRONG_KEYS;
	}

	/* type of authentication depends on schannel type */
	if (schannel_type == SEC_CHAN_RODC) {
		s->local_negotiate_flags |= NETLOGON_NEG_RODC_PASSTHROUGH;
	}

	s->requested_negotiate_flags = s->local_negotiate_flags;

	epm_creds = cli_credentials_init_anon(s);
	if (composite_nomem(epm_creds, c)) return c;

	/* allocate binding structure */
	s->binding = dcerpc_binding_dup(s, s->pipe->binding);
	if (composite_nomem(s->binding, c)) return c;

	/* request the netlogon endpoint mapping */
	epm_map_req = dcerpc_epm_map_binding_send(c, s->binding,
						  &ndr_table_netlogon,
						  epm_creds,
						  s->pipe->conn->event_ctx,
						  lp_ctx);
	if (composite_nomem(epm_map_req, c)) return c;

	composite_continue(c, epm_map_req, continue_epm_map_binding, c);
	return c;
}


/*
  Receive result of schannel key request
 */
static NTSTATUS dcerpc_schannel_key_recv(struct composite_context *c,
				TALLOC_CTX *mem_ctx,
				struct netlogon_creds_CredentialState **creds,
				uint32_t *requested_negotiate_flags)
{
	NTSTATUS status = composite_wait(c);

	if (NT_STATUS_IS_OK(status)) {
		struct schannel_key_state *s =
			talloc_get_type_abort(c->private_data,
			struct schannel_key_state);
		*creds = talloc_move(mem_ctx, &s->creds);
		*requested_negotiate_flags = s->requested_negotiate_flags;
	}

	talloc_free(c);
	return status;
}


struct auth_schannel_state {
	struct dcerpc_pipe *pipe;
	struct cli_credentials *credentials;
	uint32_t requested_negotiate_flags;
	const struct ndr_interface_table *table;
	struct loadparm_context *lp_ctx;
	uint8_t auth_level;
	struct netlogon_creds_CredentialState *creds_state;
	struct netlogon_creds_CredentialState save_creds_state;
	struct netr_Authenticator auth;
	struct netr_Authenticator return_auth;
	union netr_Capabilities capabilities;
	union netr_Capabilities client_caps;
	struct netr_LogonGetCapabilities c;
	union netr_CONTROL_QUERY_INFORMATION ctrl_info;
};


static void continue_bind_auth(struct composite_context *ctx);


/*
  Stage 2 of auth_schannel: Receive schannel key and initiate an
  authenticated bind using received credentials
 */
static void continue_schannel_key(struct composite_context *ctx)
{
	struct composite_context *auth_req;
	struct composite_context *c = talloc_get_type(ctx->async.private_data,
						      struct composite_context);
	struct auth_schannel_state *s = talloc_get_type(c->private_data,
							struct auth_schannel_state);
	NTSTATUS status;

	/* receive schannel key */
	c->status = dcerpc_schannel_key_recv(ctx,
					     s,
					     &s->creds_state,
					     &s->requested_negotiate_flags);
	status = c->status;
	if (!composite_is_ok(c)) {
		DEBUG(1, ("Failed to setup credentials: %s\n", nt_errstr(status)));
		return;
	}

	/* send bind auth request with received creds */
	cli_credentials_set_netlogon_creds(s->credentials, s->creds_state);

	auth_req = dcerpc_bind_auth_send(c, s->pipe, s->table, s->credentials, 
					 lpcfg_gensec_settings(c, s->lp_ctx),
					 DCERPC_AUTH_TYPE_SCHANNEL, s->auth_level,
					 NULL);
	if (composite_nomem(auth_req, c)) return;
	
	composite_continue(c, auth_req, continue_bind_auth, c);
}


/*
  Stage 3 of auth_schannel: Receive result of authenticated bind
  and say if we're done ok.
*/
static void continue_bind_auth(struct composite_context *ctx)
{
	struct composite_context *c = talloc_get_type(ctx->async.private_data,
						      struct composite_context);
	struct auth_schannel_state *s = talloc_get_type(c->private_data,
							struct auth_schannel_state);
	struct tevent_req *subreq;

	c->status = dcerpc_bind_auth_recv(ctx);
	if (!composite_is_ok(c)) return;

	/* if we have a AES encrypted connection, verify the capabilities */
	if (ndr_syntax_id_equal(&s->table->syntax_id,
				&ndr_table_netlogon.syntax_id)) {
		NTSTATUS status;
		ZERO_STRUCT(s->return_auth);

		s->save_creds_state = *s->creds_state;
		status = netlogon_creds_client_authenticator(&s->save_creds_state,
							     &s->auth);
		if (!NT_STATUS_IS_OK(status)) {
			composite_error(c, status);
			return;
		}

		s->c.in.server_name = talloc_asprintf(c,
						      "\\\\%s",
						      dcerpc_server_name(s->pipe));
		if (composite_nomem(s->c.in.server_name, c)) return;
		s->c.in.computer_name         = cli_credentials_get_workstation(s->credentials);
		s->c.in.credential            = &s->auth;
		s->c.in.return_authenticator  = &s->return_auth;
		s->c.in.query_level           = 1;

		s->c.out.capabilities         = &s->capabilities;
		s->c.out.return_authenticator = &s->return_auth;

		DEBUG(5, ("We established a AES connection, verifying logon "
			  "capabilities\n"));

		subreq = dcerpc_netr_LogonGetCapabilities_r_send(s,
								 c->event_ctx,
								 s->pipe->binding_handle,
								 &s->c);
		if (composite_nomem(subreq, c)) return;

		tevent_req_set_callback(subreq,
					continue_get_negotiated_capabilities,
					c);
		return;
	}

	composite_done(c);
}

static void continue_logon_control_do(struct composite_context *c);

/*
  Stage 4 of auth_schannel: Get the Logon Capabilities and verify them.
*/
static void continue_get_negotiated_capabilities(struct tevent_req *subreq)
{
	struct composite_context *c;
	struct auth_schannel_state *s;
	enum dcerpc_AuthType auth_type;
	enum dcerpc_AuthLevel auth_level;
	NTSTATUS status;

	c = tevent_req_callback_data(subreq, struct composite_context);
	s = talloc_get_type(c->private_data, struct auth_schannel_state);

	dcerpc_binding_handle_auth_info(s->pipe->binding_handle,
					&auth_type,
					&auth_level);

	/* receive rpc request result */
	c->status = dcerpc_netr_LogonGetCapabilities_r_recv(subreq, s);
	TALLOC_FREE(subreq);
	if (NT_STATUS_EQUAL(c->status, NT_STATUS_RPC_PROCNUM_OUT_OF_RANGE)) {
		if (s->creds_state->negotiate_flags & NETLOGON_NEG_SUPPORTS_AES) {
			DBG_ERR("%s: NT_STATUS_DOWNGRADE_DETECTED\n", __location__);
			composite_error(c, NT_STATUS_DOWNGRADE_DETECTED);
			return;
		} else if (s->creds_state->negotiate_flags & NETLOGON_NEG_STRONG_KEYS) {
			DBG_ERR("%s: NT_STATUS_DOWNGRADE_DETECTED\n", __location__);
			composite_error(c, NT_STATUS_DOWNGRADE_DETECTED);
			return;
		}

		/* This is probably NT */
		continue_logon_control_do(c);
		return;
	} else if (!composite_is_ok(c)) {
		return;
	}

	if (NT_STATUS_EQUAL(s->c.out.result, NT_STATUS_NOT_IMPLEMENTED)) {
		if (s->creds_state->negotiate_flags & NETLOGON_NEG_SUPPORTS_AES) {
			/* This means AES isn't supported. */
			DBG_ERR("%s: NT_STATUS_DOWNGRADE_DETECTED\n", __location__);
			composite_error(c, NT_STATUS_DOWNGRADE_DETECTED);
			return;
		}

		/* This is probably an old Samba version */
		composite_done(c);
		return;
	}

	/* verify credentials */
	status = netlogon_creds_client_verify(&s->save_creds_state,
					      &s->c.out.return_authenticator->cred,
					      auth_type,
					      auth_level);
	if (!NT_STATUS_IS_OK(status)) {
		composite_error(c, status);
		return;
	}

	*s->creds_state = s->save_creds_state;
	cli_credentials_set_netlogon_creds(s->credentials, s->creds_state);

	if (!NT_STATUS_IS_OK(s->c.out.result)) {
		composite_error(c, s->c.out.result);
		return;
	}

	/* compare capabilities */
	if (s->creds_state->negotiate_flags != s->capabilities.server_capabilities) {
		DBG_ERR("The client capabilities don't match the server "
			"capabilities: local[0x%08X] remote[0x%08X]\n",
			s->creds_state->negotiate_flags,
			s->capabilities.server_capabilities);
		composite_error(c, NT_STATUS_DOWNGRADE_DETECTED);
		return;
	}

	if ((s->requested_negotiate_flags & NETLOGON_NEG_SUPPORTS_AES) &&
	    (!(s->creds_state->negotiate_flags & NETLOGON_NEG_SUPPORTS_AES)))
	{
		DBG_ERR("%s: NT_STATUS_DOWNGRADE_DETECTED\n", __location__);
		composite_error(c, NT_STATUS_DOWNGRADE_DETECTED);
		return;
	}

	ZERO_STRUCT(s->return_auth);

	s->save_creds_state = *s->creds_state;
	status = netlogon_creds_client_authenticator(&s->save_creds_state,
						     &s->auth);
	if (!NT_STATUS_IS_OK(status)) {
		composite_error(c, status);
		return;
	}

	s->c.in.credential            = &s->auth;
	s->c.in.return_authenticator  = &s->return_auth;
	s->c.in.query_level           = 2;

	s->c.out.capabilities         = &s->client_caps;
	s->c.out.return_authenticator = &s->return_auth;

	subreq = dcerpc_netr_LogonGetCapabilities_r_send(s,
							 c->event_ctx,
							 s->pipe->binding_handle,
							 &s->c);
	if (composite_nomem(subreq, c)) return;

	tevent_req_set_callback(subreq, continue_get_client_capabilities, c);
	return;
}

static void continue_get_client_capabilities(struct tevent_req *subreq)
{
	struct composite_context *c;
	struct auth_schannel_state *s;
	enum dcerpc_AuthType auth_type;
	enum dcerpc_AuthLevel auth_level;
	NTSTATUS status;

	c = tevent_req_callback_data(subreq, struct composite_context);
	s = talloc_get_type(c->private_data, struct auth_schannel_state);

	dcerpc_binding_handle_auth_info(s->pipe->binding_handle,
					&auth_type,
					&auth_level);

	/* receive rpc request result */
	c->status = dcerpc_netr_LogonGetCapabilities_r_recv(subreq, s);
	TALLOC_FREE(subreq);
	if (NT_STATUS_EQUAL(c->status, NT_STATUS_RPC_BAD_STUB_DATA)) {
		/*
		 * unpatched Samba server, see
		 * https://bugzilla.samba.org/show_bug.cgi?id=15418
		 */
		c->status = NT_STATUS_RPC_ENUM_VALUE_OUT_OF_RANGE;
	}
	if (NT_STATUS_EQUAL(c->status, NT_STATUS_RPC_ENUM_VALUE_OUT_OF_RANGE)) {
		/*
		 * Here we know the negotiated flags were already
		 * verified with query_level=1, which means
		 * the server supported NETLOGON_NEG_SUPPORTS_AES
		 * and also NETLOGON_NEG_AUTHENTICATED_RPC
		 *
		 * As we're using DCERPC_AUTH_TYPE_SCHANNEL with
		 * DCERPC_AUTH_LEVEL_INTEGRITY or DCERPC_AUTH_LEVEL_PRIVACY
		 * we should detect a faked
		 * NT_STATUS_RPC_ENUM_VALUE_OUT_OF_RANGE
		 * with the next request as the sequence number processing
		 * gets out of sync.
		 *
		 * So we'll do a LogonControl message to check that...
		 */
		continue_logon_control_do(c);
		return;
	}
	if (!composite_is_ok(c)) {
		return;
	}

	/* verify credentials */
	status = netlogon_creds_client_verify(&s->save_creds_state,
					      &s->c.out.return_authenticator->cred,
					      auth_type,
					      auth_level);
	if (!NT_STATUS_IS_OK(status)) {
		composite_error(c, status);
		return;
	}

	if (!NT_STATUS_IS_OK(s->c.out.result)) {
		composite_error(c, s->c.out.result);
		return;
	}

	/* compare capabilities */
	if (s->requested_negotiate_flags != s->client_caps.requested_flags) {
		DBG_ERR("The client requested capabilities did not reach"
			"the server! local[0x%08X] remote[0x%08X]\n",
			s->requested_negotiate_flags,
			s->client_caps.requested_flags);
		composite_error(c, NT_STATUS_DOWNGRADE_DETECTED);
		return;
	}

	*s->creds_state = s->save_creds_state;
	cli_credentials_set_netlogon_creds(s->credentials, s->creds_state);

	composite_done(c);
}

static void continue_logon_control_done(struct tevent_req *subreq);

static void continue_logon_control_do(struct composite_context *c)
{
	struct auth_schannel_state *s = NULL;
	struct tevent_req *subreq = NULL;

	s = talloc_get_type(c->private_data, struct auth_schannel_state);

	subreq = dcerpc_netr_LogonControl_send(s,
					       c->event_ctx,
					       s->pipe->binding_handle,
					       s->c.in.server_name,
					       NETLOGON_CONTROL_QUERY,
					       2,
					       &s->ctrl_info);
	if (composite_nomem(subreq, c)) return;

	tevent_req_set_callback(subreq, continue_logon_control_done, c);
}

static void continue_logon_control_done(struct tevent_req *subreq)
{
	struct composite_context *c = NULL;
	struct auth_schannel_state *s = NULL;
	WERROR werr;

	c = tevent_req_callback_data(subreq, struct composite_context);
	s = talloc_get_type(c->private_data, struct auth_schannel_state);

	/* receive rpc request result */
	c->status = dcerpc_netr_LogonControl_recv(subreq, s, &werr);
	TALLOC_FREE(subreq);
	if (!NT_STATUS_IS_OK(c->status)) {
		DBG_ERR("%s: NT_STATUS_DOWNGRADE_DETECTED\n", __location__);
		composite_error(c, NT_STATUS_DOWNGRADE_DETECTED);
		return;
	}

	if (!W_ERROR_EQUAL(werr, WERR_NOT_SUPPORTED)) {
		DBG_ERR("%s: NT_STATUS_DOWNGRADE_DETECTED\n", __location__);
		composite_error(c, NT_STATUS_DOWNGRADE_DETECTED);
		return;
	}

	composite_done(c);
}

/*
  Initiate schannel authentication request
*/
struct composite_context *dcerpc_bind_auth_schannel_send(TALLOC_CTX *tmp_ctx, 
							 struct dcerpc_pipe *p,
							 const struct ndr_interface_table *table,
							 struct cli_credentials *credentials,
							 struct loadparm_context *lp_ctx,
							 uint8_t auth_level)
{
	struct composite_context *c;
	struct auth_schannel_state *s;
	struct composite_context *schan_key_req;

	/* composite context allocation and setup */
	c = composite_create(tmp_ctx, p->conn->event_ctx);
	if (c == NULL) return NULL;
	
	s = talloc_zero(c, struct auth_schannel_state);
	if (composite_nomem(s, c)) return c;
	c->private_data = s;

	/* store parameters in the state structure */
	s->pipe        = p;
	s->credentials = credentials;
	s->table       = table;
	s->auth_level  = auth_level;
	s->lp_ctx      = lp_ctx;

	/* start getting schannel key first */
	schan_key_req = dcerpc_schannel_key_send(c, p, credentials, lp_ctx);
	if (composite_nomem(schan_key_req, c)) return c;

	composite_continue(c, schan_key_req, continue_schannel_key, c);
	return c;
}


/*
  Receive result of schannel authentication request
*/
NTSTATUS dcerpc_bind_auth_schannel_recv(struct composite_context *c)
{
	NTSTATUS status = composite_wait(c);
	
	talloc_free(c);
	return status;
}
