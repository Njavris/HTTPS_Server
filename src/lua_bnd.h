#ifndef __LUA_BND_H__
#define __LUA_BND_H__

#include <sqlite3.h>
#include <lua.hpp>

#include <server.h>

struct LuaThreadEnv {
	lua_State* L;
	sqlite3* db;

	LuaThreadEnv();
	~LuaThreadEnv();
};

void push_request_to_lua(lua_State*, const HttpRequest&);

extern thread_local LuaThreadEnv threadLua;

#endif // __LUA_BND_H__
