/*
 *
 * (C) 2024-2025 Allen Day
 * (C) 2003-2025 Anope Team
 *
 * Contact us at team@anope.org
 *
 * Please read COPYING and README for further details.
 */

#include "module.h"
#include "modules/sql.h"
#include "protocol.h"

namespace
{
	Module* me = nullptr;

	struct MetadataField final
	{
		Anope::string name;
		Anope::string set_query;
		Anope::string get_query;
		Anope::string result_column{"value"};
		bool sync_on_login{true};
		bool local_only{true};
	};

	class SQLMetadataQuery final
		: public SQL::Interface
	{
	public:
		SQLMetadataQuery() : SQL::Interface(me) { }

		void OnResult(const SQL::Result &) override
		{
			delete this;
		}

		void OnError(const SQL::Result &r) override
		{
			Log(this->owner) << "sql_metadata: Error executing query " << r.GetQuery().query << ": " << r.GetError();
			delete this;
		}
	};

	class SQLMetadataFetch final
		: public SQL::Interface
	{
		Reference<User> user;
		MetadataField field;

	public:
		SQLMetadataFetch(User *u, const MetadataField &f)
			: SQL::Interface(me)
			, user(u)
			, field(f)
		{
		}

		void OnResult(const SQL::Result &r) override
		{
			if (!user || r.Rows() == 0)
			{
				delete this;
				return;
			}

			Anope::string value;
			try
			{
				value = r.Get(0, field.result_column);
			}
			catch (const SQL::Exception &)
			{
				delete this;
				return;
			}

			if (!value.empty())
				IRCD->SendMetadata(user, field.name, value);

			delete this;
		}

		void OnError(const SQL::Result &r) override
		{
			Log(this->owner) << "sql_metadata: Error executing query " << r.GetQuery().query << ": " << r.GetError();
			delete this;
		}
	};
}

class ModuleSQLMetadata final
	: public Module
{
	Anope::string engine;
	ServiceReference<SQL::Provider> SQL;
	std::map<Anope::string, MetadataField> fields;
	Anope::string lookup_query;
	Anope::string lookup_result_column{"user_id"};
	time_t lookup_cache_ttl{300};

	struct CachedId
	{
		Anope::string value;
		time_t added{0};
	};

	std::map<Anope::string, CachedId> lookup_cache;

	void FillTokens(SQL::Query &q, User *u, const Anope::string &key, const Anope::string &value, const Anope::string &userid) const
	{
		if (u)
		{
			q.SetValue("nick", u->nick);
			q.SetValue("uid", u->GetUID());
			q.SetValue("ident", u->GetIdent());
			q.SetValue("vhost", u->GetDisplayedHost());
			if (u->Account())
			{
				q.SetValue("account", u->Account()->display);
				q.SetValue("account_id", stringify(u->Account()->GetId()));
			}
			else
			{
				q.SetValue("account", "");
				q.SetValue("account_id", "");
			}
		}
		else
		{
			q.SetValue("nick", "");
			q.SetValue("uid", "");
			q.SetValue("ident", "");
			q.SetValue("vhost", "");
			q.SetValue("account", "");
			q.SetValue("account_id", "");
		}

		q.SetValue("key", key);
		q.SetValue("value", value);
		q.SetValue("user_id", userid);
	}

	Anope::string ResolveExternalId(User *u)
	{
		if (!u || lookup_query.empty() || !SQL)
			return "";

		NickCore *nc = u->Account();
		if (!nc)
			return "";

		Anope::string cache_key = nc->display;
		if (lookup_cache_ttl > 0)
		{
			auto it = lookup_cache.find(cache_key);
			if (it != lookup_cache.end() && (Anope::CurTime - it->second.added) <= lookup_cache_ttl)
				return it->second.value;
		}

		SQL::Query q(lookup_query);
		FillTokens(q, u, "", "", "");
		SQL::Result r = SQL->RunQuery(q);
		if (!r.GetError().empty() || r.Rows() == 0)
			return "";

		Anope::string value;
		try
		{
			value = r.Get(0, lookup_result_column);
		}
		catch (const SQL::Exception &)
		{
			return "";
		}

		if (!value.empty() && lookup_cache_ttl > 0)
			lookup_cache[cache_key] = {value, Anope::CurTime};

		return value;
	}

	void BindCommon(SQL::Query &q, User *u, const Anope::string &key, const Anope::string &value) 
	{
		Anope::string external_id = ResolveExternalId(u);
		FillTokens(q, u, key, value, external_id);
	}

public:
	ModuleSQLMetadata(const Anope::string &modname, const Anope::string &creator)
		: Module(modname, creator, EXTRA | VENDOR)
	{
		me = this;
	}

	void OnReload(Configuration::Conf *conf) override
	{
		Configuration::Block *config = conf->GetModule(this);
		engine = config->Get<const Anope::string>("engine", "mysql/main");
		SQL = ServiceReference<SQL::Provider>("SQL::Provider", engine);

		fields.clear();
		lookup_cache.clear();
		lookup_query = config->Get<const Anope::string>("lookup_query");
		lookup_result_column = config->Get<const Anope::string>("lookup_result_column", "user_id");
		lookup_cache_ttl = config->Get<time_t>("lookup_cache_ttl", "5m");

		for (int i = 0; i < config->CountBlock("field"); ++i)
		{
			Configuration::Block *block = config->GetBlock("field", i);
			MetadataField field;
			field.name = block->Get<const Anope::string>("name");
			field.set_query = block->Get<const Anope::string>("set", "");
			field.get_query = block->Get<const Anope::string>("get", "");
			field.result_column = block->Get<const Anope::string>("result_column", "value");
			field.sync_on_login = block->Get<bool>("sync_on_login", true);
			field.local_only = block->Get<bool>("local_only", true);

			if (field.name.empty())
				throw ConfigException("sql_metadata: field block missing name");

			fields[field.name] = field;
		}
	}

	void OnUserMetadata(User *u, const Anope::string &key, const Anope::string &value) override
	{
		if (!u || !u->Account() || fields.empty() || !SQL)
			return;

		auto it = fields.find(key);
		if (it == fields.end())
			return;

		const MetadataField &field = it->second;
		if (field.set_query.empty())
			return;
		if (field.local_only && u->server != Me)
			return;

		SQL::Query q(field.set_query);
		BindCommon(q, u, key, value);
		SQL->Run(new SQLMetadataQuery(), q);
	}

	void OnUserLogin(User *u) override
	{
		if (!u || !u->Account() || fields.empty() || !SQL)
			return;

		for (const auto &it : fields)
		{
			const MetadataField &field = it.second;
			if (field.get_query.empty() || !field.sync_on_login)
				continue;
			if (field.local_only && u->server != Me)
				continue;

			SQL::Query q(field.get_query);
			BindCommon(q, u, field.name, "");
			SQL->Run(new SQLMetadataFetch(u, field), q);
		}
	}
};

MODULE_INIT(ModuleSQLMetadata)
