/*
 * Minimal async HTTP client based on libcurl.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* RequiredLibraries: curl */

#include "module.h"
#include "modules/extra/http_client.h"

extern "C" {
#include <curl/curl.h>
}

#include <deque>

using namespace HTTP;

namespace
{
	class CurlHttpClient;
	CurlHttpClient *client = nullptr;

	struct Job
	{
		Interface *cb{nullptr};
		RequestOptions opts;
	};

	struct JobResult
	{
		Interface *cb{nullptr};
		RequestOptions opts;
		Response res;
	};

	class Worker final : public Thread
	{
		CurlHttpClient &owner;
		Mutex lock;
		Condition cond;
		std::deque<Job> jobs;
		std::deque<JobResult> results;
		bool stopping{false};

		static size_t WriteCallback(void *contents, size_t size, size_t nmemb, std::string *userp)
		{
			userp->append(static_cast<char *>(contents), size * nmemb);
			return size * nmemb;
		}

		bool Dequeue(Job &job)
		{
			lock.Lock();
			while (jobs.empty() && !stopping)
				cond.Wait();
			if (stopping)
			{
				lock.Unlock();
				return false;
			}
			job = jobs.front();
			jobs.pop_front();
			lock.Unlock();
			return true;
		}

		void PushResult(const JobResult &jr)
		{
			lock.Lock();
			results.push_back(jr);
			lock.Unlock();
			this->Notify();
		}

		void ProcessResults()
		{
			std::deque<JobResult> local;
			lock.Lock();
			local.swap(results);
			lock.Unlock();

			for (const JobResult &jr : local)
			{
				if (!jr.cb)
					continue;
				jr.cb->OnResult(jr.opts, jr.res);
			}
		}

	public:
		explicit Worker(CurlHttpClient &c) : owner(c)
		{
			Start();
		}

		~Worker() override
		{
			if (!stopping)
				Stop();
		}

		void Queue(const Job &job)
		{
			lock.Lock();
			jobs.push_back(job);
			lock.Unlock();
			cond.Wakeup();
		}

		void Stop()
		{
			if (stopping)
				return;
			lock.Lock();
			stopping = true;
			lock.Unlock();
			cond.Wakeup();
			SetExitState();
		}

		void Run() override
		{
			for (;;)
			{
				Job job;
				if (!Dequeue(job))
					break;

				Response res;
				res.ok = false;

				CURL *curl = curl_easy_init();
				if (!curl)
				{
					res.error = "curl_easy_init failed";
					PushResult(JobResult{job.cb, job.opts, res});
					continue;
				}

				struct curl_slist *headers = NULL;
			for (const auto &h : job.opts.headers)
				headers = curl_slist_append(headers, h.c_str());

			curl_easy_setopt(curl, CURLOPT_URL, job.opts.url.c_str());
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
			if (!job.opts.user_agent.empty())
				curl_easy_setopt(curl, CURLOPT_USERAGENT, job.opts.user_agent.c_str());
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);
				curl_easy_setopt(curl, CURLOPT_TIMEOUT, job.opts.timeout_secs > 0 ? job.opts.timeout_secs : 5);
				curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, job.opts.verify_peer ? 1L : 0L);
				curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, job.opts.verify_host ? 2L : 0L);
				if (!job.opts.ca_info.empty())
					curl_easy_setopt(curl, CURLOPT_CAINFO, job.opts.ca_info.c_str());

				if (job.opts.follow_redirects)
				{
					curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
					curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
				}
				else
				{
					curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
					curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 0L);
				}

				CURLcode code = curl_easy_perform(curl);
				curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.http_code);

				if (code == CURLE_OK && res.http_code >= 200 && res.http_code < 300)
					res.ok = true;
				else
					res.error = code == CURLE_OK ? "http_" + stringify(res.http_code) : curl_easy_strerror(code);

				curl_slist_free_all(headers);
				curl_easy_cleanup(curl);

				PushResult(JobResult{job.cb, job.opts, res});
			}
		}

		void OnNotify() override
		{
			ProcessResults();
		}
	};

	class CurlHttpClient final : public Client
	{
		Worker *worker{nullptr};

	public:
		CurlHttpClient(Module *o) : Client(o)
		{
			worker = new Worker(*this);
		}

		~CurlHttpClient() override
		{
			if (worker)
			{
				worker->Stop();
				worker->Join();
				delete worker;
				worker = nullptr;
			}
		}

		void Get(Interface *cb, const RequestOptions &req) override
		{
			if (!worker)
				return;

			worker->Queue(Job{cb, req});
		}
	};
}

class ModuleHTTPClient final : public Module
{
	CurlHttpClient http_client;

public:
	ModuleHTTPClient(const Anope::string &modname, const Anope::string &creator)
		: Module(modname, creator, EXTRA | VENDOR)
		, http_client(this)
	{
		client = &http_client;
	}

	~ModuleHTTPClient() override
	{
		client = nullptr;
	}
};

MODULE_INIT(ModuleHTTPClient)
