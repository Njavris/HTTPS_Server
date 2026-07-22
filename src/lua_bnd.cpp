#include <iostream>

#include <openssl/sha.h>
#include <openssl/rand.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <lua_bnd.h>
#include <misc.h>

int l_send_response(lua_State* L) {
	int fd = (int)luaL_checkinteger(L, 1);
	luaL_checktype(L, 2, LUA_TTABLE);

	HttpResponse res;
	res.client_fd = fd;

	lua_getfield(L, 2, "status");
	res.status_code = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : 200;
	lua_pop(L, 1);

	lua_getfield(L, 2, "body");
	if (lua_isstring(L, -1)) {
		size_t len;
		const char* data = lua_tolstring(L, -1, &len);
		res.body.assign(data, len);
	}
	lua_pop(L, 1);

	lua_getfield(L, 2, "headers");
	if (lua_istable(L, -1)) {
		lua_pushnil(L);
		while (lua_next(L, -2)) {
			if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
				res.headers.push_back({lua_tostring(L, -2), lua_tostring(L, -1)});
			}
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);

	global_send_queue.push(std::move(res));
	char wake = 1;
	(void)!write(wakePipe[1], &wake, 1);
	return 0;
}

void push_request_to_lua(lua_State* L, const HttpRequest& req) {
	lua_newtable(L);
	lua_pushstring(L, req.method.c_str());
	lua_setfield(L, -2, "method");
	lua_pushstring(L, req.uri.c_str());
	lua_setfield(L, -2, "uri");
	lua_pushlstring(L, req.body.data(), req.body.size());
	lua_setfield(L, -2, "body");

	lua_newtable(L);
	for (auto const& it : req.headers) {
		lua_pushstring(L, it.second.c_str());
		lua_setfield(L, -2, it.first.c_str());
	}
	lua_setfield(L, -2, "headers");
}

int l_db_exec(lua_State* L) {
	const char* sql = luaL_checkstring(L, 1);
	lua_getfield(L, LUA_REGISTRYINDEX, "db_handle");
	sqlite3* db = (sqlite3*)lua_touserdata(L, -1);
	lua_pop(L, 1);

	if (!db) {
		return luaL_error(L, "Database handle is NULL");
	}
	linfo << "Executing SQL on Handle [" << (void*)db << "]" << sql << log::endl;

	char* zErrMsg = nullptr;
	int rc = sqlite3_exec(db, sql, nullptr, nullptr, &zErrMsg);

	if (rc != SQLITE_OK) {
		lua_pushboolean(L, false);
		lua_pushstring(L, zErrMsg ? zErrMsg : "Unknown SQL error");
		if (zErrMsg)
			sqlite3_free(zErrMsg);
		return 2;
	}

	lua_pushboolean(L, true);
	return 1;
}

int l_db_query(lua_State* L) {
	const char* sql = luaL_checkstring(L, 1);

	lua_getfield(L, LUA_REGISTRYINDEX, "db_handle");
	sqlite3* db = (sqlite3*)lua_touserdata(L, -1);
	lua_pop(L, 1);
	if (!db) {
		return luaL_error(L, "Database handle is NULL");
	}
	linfo << "SQL query on Handle [" << (void*)db << "]" << sql << log::endl;

	sqlite3_stmt* stmt;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		const char* errMsg = sqlite3_errmsg(db);
		lua_pushnil(L);
		lua_pushstring(L, errMsg);
		return 2;
	}

	lua_newtable(L);
	int row_idx = 1;

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		lua_pushinteger(L, row_idx++);
		lua_newtable(L);

		int cols = sqlite3_column_count(stmt);
		for (int i = 0; i < cols; i++) {
			const char* name = sqlite3_column_name(stmt, i);
			int type = sqlite3_column_type(stmt, i);

			if (type == SQLITE_BLOB) {
				const void* blob_data = sqlite3_column_blob(stmt, i);
				int blob_len = sqlite3_column_bytes(stmt, i);
				lua_pushlstring(L, (const char*)blob_data, blob_len);
			} else {
				const char* val = (const char*)sqlite3_column_text(stmt, i);
				lua_pushstring(L, val ? val : "");
			}
			lua_setfield(L, -2, name);
		}
		lua_settable(L, -3);
	}

	sqlite3_finalize(stmt);
	return 1;
}

std::string to_hex_string(const unsigned char* data, size_t len) {
	std::stringstream ss;
	ss << std::hex << std::setfill('0');
	for (size_t i = 0; i < len; ++i) {
		ss << std::setw(2) << static_cast<int>(data[i]);
	}
	return ss.str();
}

int l_hash_password(lua_State* L) {
	std::string password = luaL_checkstring(L, 1);
	unsigned char salt[16];
	if (RAND_bytes(salt, sizeof(salt)) != 1) 
		return luaL_error(L, "Random generation failed");

	std::string salted_pass = password + std::string((char*)salt, 16);
	unsigned char hash[SHA256_DIGEST_LENGTH];
	SHA256((unsigned char*)salted_pass.c_str(), salted_pass.length(), hash);

	std::string result = to_hex_string(salt, 16) + ":" + to_hex_string(hash, SHA256_DIGEST_LENGTH);

	lua_pushstring(L, result.c_str());
	return 1;
}

int l_verify_password(lua_State* L) {
	std::string provided_password = luaL_checkstring(L, 1);
	std::string stored_entry = luaL_checkstring(L, 2);
	size_t delimiter = stored_entry.find(':');
	if (delimiter == std::string::npos) {
		lua_pushboolean(L, false);
		return 1;
	}

	std::string salt_hex = stored_entry.substr(0, delimiter);
	std::string original_hash = stored_entry.substr(delimiter + 1);

	std::string salt_bin;
	for (size_t i = 0; i < salt_hex.length(); i += 2) {
		std::string byteString = salt_hex.substr(i, 2);
		char byte = (char)strtol(byteString.c_str(), nullptr, 16);
		salt_bin.push_back(byte);
	}

	std::string test_salted = provided_password + salt_bin;
	unsigned char test_hash[SHA256_DIGEST_LENGTH];
	SHA256((unsigned char*)test_salted.c_str(), test_salted.length(), test_hash);

	lua_pushboolean(L, to_hex_string(test_hash, SHA256_DIGEST_LENGTH) == original_hash);
	return 1;
}

int l_generate_token(lua_State* L) {
	unsigned char buf[32];
	if (RAND_bytes(buf, sizeof(buf)) != 1) {
		return luaL_error(L, "System random generation failed");
	}

	std::string hex = to_hex_string(buf, 32);
	lua_pushstring(L, hex.c_str());
	return 1;
}

int l_save_file(lua_State* L) {
	const char* filename = luaL_checkstring(L, 1);
	size_t len;
	const char* data = luaL_checklstring(L, 2, &len);

	std::string safeFilename = std::filesystem::path(filename).filename().string();
	if (safeFilename.empty() || safeFilename == "." || safeFilename == "..") {
		lua_pushboolean(L, false);
		return 1;
	}

	std::ofstream out(std::string("uploads/") + safeFilename, std::ios::binary);
	if (!out) {
		lua_pushboolean(L, false);
		return 1;
	}
	out.write(data, len);
	out.close();
	lua_pushboolean(L, true);
	return 1;
}

int l_db_exec_blob(lua_State* L) {
	bool ret = true;
	const char* sql = luaL_checkstring(L, 1);
	lua_getfield(L, LUA_REGISTRYINDEX, "db_handle");
	sqlite3* db = (sqlite3*)lua_touserdata(L, -1);

	size_t len;
	const char* data = lua_tolstring(L, 2, &len); 

	sqlite3_stmt* stmt;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
		const char* errMsg = sqlite3_errmsg(db);
		lua_pushnil(L);
		lua_pushstring(L, errMsg);
		return 2;
	}

	sqlite3_bind_blob(stmt, 1, data, len, SQLITE_TRANSIENT);

	if (sqlite3_step(stmt) != SQLITE_DONE) {
		ret = false;
	}

	sqlite3_finalize(stmt);
	lua_pushboolean(L, ret);
	return 1;
}

int l_proxy_to_backend(lua_State* L) {
	int client_fd = (int)luaL_checkinteger(L, 1);
	const char* target_host = luaL_checkstring(L, 2);
	int target_port = (int)luaL_checkinteger(L, 3);
	const char* path_override = luaL_optstring(L, 4, nullptr);
	luaL_checktype(L, 5, LUA_TTABLE);

	lua_getfield(L, 5, "method");
	std::string method = lua_isstring(L, -1) ? lua_tostring(L, -1) : "GET";
	lua_pop(L, 1);

	lua_getfield(L, 5, "uri");
	std::string orig_uri = lua_isstring(L, -1) ? lua_tostring(L, -1) : "/";
	lua_pop(L, 1);

	std::string target_path = (path_override && path_override[0] != '\0') ? path_override : orig_uri;

	lua_getfield(L, 5, "body");
	std::string body;
	if (lua_isstring(L, -1)) {
		size_t len;
		const char* b = lua_tolstring(L, -1, &len);
		body.assign(b, len);
	}
	lua_pop(L, 1);

	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		lua_pushboolean(L, false);
		return 1;
	}

	struct sockaddr_in target_addr{};
	target_addr.sin_family = AF_INET;
	target_addr.sin_port = htons(target_port);
	inet_pton(AF_INET, target_host, &target_addr.sin_addr);

	if (connect(sock, (struct sockaddr*)&target_addr, sizeof(target_addr)) < 0) {
		close(sock);
		HttpResponse res;
		res.client_fd = client_fd;
		res.status_code = 502;
		res.body = "502 Bad Gateway: Could not connect to backend server";
		global_send_queue.push(std::move(res));
		char wake = 1;
		(void)!write(wakePipe[1], &wake, 1);
		lua_pushboolean(L, false);
		return 1;
	}

	std::ostringstream req_stream;
	req_stream << method << " " << target_path << " HTTP/1.1\r\n";
	req_stream << "Host: " << target_host << ":" << target_port << "\r\n";
	req_stream << "Connection: close\r\n";

	lua_getfield(L, 5, "headers");
	if (lua_istable(L, -1)) {
		lua_pushnil(L);
		while (lua_next(L, -2)) {
			if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
				std::string k = lua_tostring(L, -2);
				std::string v = lua_tostring(L, -1);
				if (k != "HOST" && k != "CONNECTION" && k != "Host" && k != "Connection") {
					req_stream << k << ": " << v << "\r\n";
				}
			}
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);

	if (!body.empty()) {
		req_stream << "Content-Length: " << body.size() << "\r\n";
	}
	req_stream << "\r\n";
	req_stream << body;

	std::string req_data = req_stream.str();
	send(sock, req_data.data(), req_data.size(), 0);

	std::string raw_response;
	char buf[4096];
	ssize_t bytes_read;
	while ((bytes_read = recv(sock, buf, sizeof(buf), 0)) > 0) {
		raw_response.append(buf, bytes_read);
	}
	close(sock);

	HttpResponse res;
	res.client_fd = client_fd;
	res.status_code = 502;

	size_t header_end = raw_response.find("\r\n\r\n");
	if (header_end != std::string::npos) {
		std::string header_part = raw_response.substr(0, header_end);
		res.body = raw_response.substr(header_end + 4);

		std::istringstream hstream(header_part);
		std::string status_line;
		if (std::getline(hstream, status_line)) {
			std::istringstream sl_stream(status_line);
			std::string http_ver;
			int code = 200;
			sl_stream >> http_ver >> code;
			if (code > 0) res.status_code = code;
		}

		std::string line;
		while (std::getline(hstream, line)) {
			if (!line.empty() && line.back() == '\r') line.pop_back();
			size_t colon = line.find(':');
			if (colon != std::string::npos) {
				std::string hk = line.substr(0, colon);
				std::string hv = line.substr(colon + 1);
				if (!hv.empty() && hv[0] == ' ') hv.erase(0, 1);
				if (hk != "Transfer-Encoding" && hk != "Content-Length") {
					res.headers.push_back({hk, hv});
				}
			}
		}
	} else {
		res.body = raw_response;
		res.status_code = 200;
	}

	global_send_queue.push(std::move(res));
	char wake = 1;
	(void)!write(wakePipe[1], &wake, 1);

	lua_pushboolean(L, true);
	return 1;
}

LuaThreadEnv::LuaThreadEnv() {
	L = luaL_newstate();
	luaL_openlibs(L);
	std::string serveFile = globalCfg.getConfigFS("server", "serve_file", "");
	std::string sqlDbFile = globalCfg.getConfigFS("server", "sql_db_file", "");

	if (sqlite3_open(sqlDbFile.c_str(), &db) != SQLITE_OK) {
		lerr << "SQL Open Error: " << sqlite3_errmsg(db) << log::endl;
	}

	lua_pushlightuserdata(L, db);
	lua_setfield(L, LUA_REGISTRYINDEX, "db_handle");

	lua_register(L, "db_exec", l_db_exec);
	lua_register(L, "db_query", l_db_query);
	lua_register(L, "db_exec_blob", l_db_exec_blob);
	lua_register(L, "send_response", l_send_response);
	lua_register(L, "hash_password", l_hash_password);
	lua_register(L, "verify_password", l_verify_password);
	lua_register(L, "generate_token", l_generate_token);
	lua_register(L, "save_file", l_save_file);
	lua_register(L, "proxy_to_backend", l_proxy_to_backend);

	if (luaL_dofile(L, serveFile.c_str()) != LUA_OK) {
		lerr << "Lua Load Error: " << lua_tostring(L, -1) << log::endl;
	}
}

LuaThreadEnv::~LuaThreadEnv() {
	if (db) sqlite3_close(db);
	if (L) lua_close(L);
}

thread_local LuaThreadEnv threadLua;
