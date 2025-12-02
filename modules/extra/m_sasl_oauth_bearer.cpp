// Anope module: SASL PLAIN validation via OAuth/JWT
//
// Treats the SASL password as a JWT access token. Validates it against a
// configured OAuth provider (/oauth/me endpoint) and, if the username claim
// matches the SASL account, marks the auth as successful.
//
// Config (modules.conf):
// module
// {
//     name = "m_sasl_oauth_bearer"
//     me_url = "https://example.invalid/oauth/me"
//     preferred_claim = "preferred_username"
//     fallback_claim = "user_login"
//     auth_header_prefix = "Authorization: Bearer "
//     timeout = 5
//     follow_redirects = false
//     verify_peer = true
//     verify_host = true
//     ca_info = ""
// }
//
// SPDX-License-Identifier: GPL-2.0-only

/* RequiredLibraries: curl */

#include "module.h"
#include "modules/sasl.h"
#include "modules/extra/http_client.h"

extern "C" {
#include "yyjson.c"
}

class SaslOAuthBearerModule final : public Module
{
	Anope::string me_url;
	Anope::string preferred_claim{"preferred_username"};
	Anope::string fallback_claim{"user_login"};
	Anope::string auth_header_prefix{"Authorization: Bearer "};
	int timeout_secs{5};
	bool follow_redirects{false};
	bool verify_peer{true};
	bool verify_host{true};
	Anope::string ca_info;
	ServiceReference<HTTP::Client> http_client;

	Anope::string yyjson_get_astr(yyjson_val *obj, const char *key)
	{
		const char *str = yyjson_get_str(yyjson_obj_get(obj, key));
		return str ? Anope::string(str) : Anope::string("");
	}

public:
	Anope::string ExtractUsername(const std::string &body)
	{
		yyjson_doc *doc = yyjson_read(body.c_str(), body.length(), 0);
		if (!doc)
			return "";
		yyjson_val *root = yyjson_doc_get_root(doc);
		if (!yyjson_is_obj(root))
		{
			yyjson_doc_free(doc);
			return "";
		}

		Anope::string user = yyjson_get_astr(root, preferred_claim.c_str());
		if (user.empty() && !fallback_claim.empty())
			user = yyjson_get_astr(root, fallback_claim.c_str());

		yyjson_doc_free(doc);
		return user;
	}

	SaslOAuthBearerModule(const Anope::string &modname, const Anope::string &creator)
		: Module(modname, creator, THIRD)
		, http_client("HTTP::Client", "http_client")
	{
	}

	void OnReload(Configuration::Conf *conf) anope_override
	{
		Configuration::Block *block = conf->GetModule(this);
		me_url = block->Get<Anope::string>("me_url", me_url);
		preferred_claim = block->Get<Anope::string>("preferred_claim", preferred_claim);
		fallback_claim = block->Get<Anope::string>("fallback_claim", fallback_claim);
		auth_header_prefix = block->Get<Anope::string>("auth_header_prefix", auth_header_prefix);
		timeout_secs = block->Get<int>("timeout", "5");
		follow_redirects = block->Get<bool>("follow_redirects", false);
		verify_peer = block->Get<bool>("verify_peer", true);
		verify_host = block->Get<bool>("verify_host", true);
		ca_info = block->Get<Anope::string>("ca_info");

		if (!http_client)
			http_client = ServiceReference<HTTP::Client>("HTTP::Client", "http_client");
	}

	void OnCheckAuthentication(User *, IdentifyRequest *req) anope_override
	{
		if (!req)
			return;

		if (!anope_dynamic_static_cast<SASL::IdentifyRequest *>(req))
			return;

		if (me_url.empty())
		{
			Log(LOG_DEBUG) << "sasl_oauth_bearer: me_url not configured; skipping";
			return;
		}

		if (!http_client)
			return;

		// Treat SASL password as JWT access token
		Anope::string token = req->GetPassword();
		if (token.empty())
			return;

		class Callback final : public HTTP::Interface
		{
			SaslOAuthBearerModule *mod;
			IdentifyRequest *req;
		public:
			Callback(SaslOAuthBearerModule *m, IdentifyRequest *r) : mod(m), req(r)
			{
				if (req && mod)
					req->Hold(mod);
			}

			~Callback()
			{
				if (req && mod)
					req->Release(mod);
			}

			void OnResult(const HTTP::RequestOptions &, const HTTP::Response &res) override
			{
				if (!mod || !req)
					return;

				if (!res.ok)
				{
					Log(LOG_DEBUG) << "sasl_oauth_bearer: HTTP GET failed for account " << req->GetAccount()
								   << " error=" << res.error;
					return;
				}

				Anope::string claimed = mod->ExtractUsername(res.body);
				if (!claimed.empty() && req->GetAccount().equals_ci(claimed))
				{
					Log(LOG_DEBUG) << "sasl_oauth_bearer: SUCCESS for account=" << req->GetAccount();
					req->Success(mod);
				}
				else
				{
					Log(LOG_DEBUG) << "sasl_oauth_bearer: account mismatch/empty claim for account=" << req->GetAccount();
				}
			}
		};

		HTTP::RequestOptions opts;
		opts.url = me_url;
		opts.headers.push_back(auth_header_prefix + token);
		opts.headers.push_back("Accept: application/json");
		opts.timeout_secs = timeout_secs;
		opts.follow_redirects = follow_redirects;
		opts.verify_peer = verify_peer;
		opts.verify_host = verify_host;
		opts.ca_info = ca_info;

		http_client->Get(new Callback(this, req), opts);
	}

	~SaslOAuthBearerModule() { }
};

MODULE_INIT(SaslOAuthBearerModule)
