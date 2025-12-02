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
		int timeout_secs{5};
		Anope::string user_agent;
		bool follow_redirects{false};
		bool verify_peer{true};
		bool verify_host{true};
		Anope::string ca_info;
	};

	struct Response
	{
		bool ok{false};
		long http_code{0};
		std::string body;
		Anope::string error;
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
