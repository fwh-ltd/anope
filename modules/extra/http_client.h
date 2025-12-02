/*
 *
 * Minimal async HTTP client service for modules.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include "module.h"

namespace HTTP
{
	struct RequestOptions
	{
		Anope::string url;
		std::vector<Anope::string> headers;
		int timeout_secs;
		Anope::string user_agent;
		bool follow_redirects;
		bool verify_peer;
		bool verify_host;
		Anope::string ca_info;
		RequestOptions() : timeout_secs(5), follow_redirects(false), verify_peer(true), verify_host(true) {}
	};

	struct Response
	{
		bool ok;
		long http_code;
		std::string body;
		Anope::string error;
		Response() : ok(false), http_code(0) {}
	};

	class Interface
	{
	 public:
		virtual ~Interface() = default;
		virtual void OnResult(const RequestOptions &req, const Response &res) = 0;
	};

	class Client : public Service
	{
	 public:
		Client(Module *o) : Service(o, "HTTP::Client", "http_client") { }
		virtual void Get(Interface *cb, const RequestOptions &req) = 0;
	};
}
