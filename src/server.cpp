#include <iostream>

#include <unistd.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <poll.h>

#include <server.h>
#include <lua_bnd.h>


SendQueue global_send_queue;
int wakePipe[2]; // [0] = read, [1] = write

Server::Server(Config &cfg) :
			fd(-1), ctx(nullptr),
			numThreads(cfg.getConfigU32("server", "num_threads", 4)), pool(numThreads) {
	struct tls_config *config;
	uint32_t protocolsCfg;

	protocols = cfg.getConfig("ssl", "protocols", "all");
	certPath = cfg.getConfigFS("ssl", "cert_file", "");
	keyPath = cfg.getConfigFS("ssl", "key_file", "");
	port = cfg.getConfigU32("server", "port", 443);
	timeout = cfg.getConfigU32("server", "keep_alive", 30);
	maxConnections = cfg.getConfigU32("server", "max_connections", 30);
	maxReqSize = cfg.getConfigU32("server", "max_req_size", 1024 * 1024); 
	serveFile = cfg.getConfigFS("server", "serve_file", "");
	sqlDbFile = cfg.getConfigFS("server", "sql_db_file", "");

	linfo << "Launching server on port " << port << log::endl;
	linfo << "Keys: " << log::endl;
	linfo << "\t" << certPath << log::endl;
	linfo << "\t" << keyPath << log::endl;
	linfo << "Security option: " << protocols << log::endl;
	linfo << "Max connection count: " << maxConnections << log::endl;
	linfo << "Worker pool size: " << numThreads << log::endl;
	linfo << "Max request size: " << maxReqSize << log::endl;
	linfo << "Serving file: " << serveFile << log::endl;

	if (keyPath.empty() || certPath.empty()) {
		lerr << "Failed to find ssl keys" << log::endl;
		exit(-1);
	}


	tls_init();
	config = tls_config_new();
	tls_config_parse_protocols(&protocolsCfg, protocols.c_str());
	tls_config_set_protocols(config, protocolsCfg);

	if (tls_config_set_cert_file(config, certPath.data()) != 0 ||
			tls_config_set_key_file(config, keyPath.data()) != 0) {
		lerr << "Cert/Key error: " << tls_config_error(config) << log::endl;
		exit(-1);
	}

	ctx = tls_server();
	tls_configure(ctx, config);
	tls_config_free(config);

	fd = socket(AF_INET, SOCK_STREAM, 0);
	int opt = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	fcntl(fd, F_SETFL, O_NONBLOCK);

	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;
	bind(fd, (struct sockaddr*)&addr, sizeof(addr));
	listen(fd, SOMAXCONN);

	if (pipe(wakePipe) == -1) {
		lerr << "Failed to open pipe" << log::endl;
		perror("pipe");
		exit(-1);
	}
	fcntl(wakePipe[0], F_SETFL, O_NONBLOCK);
}

Server::~Server() {
	for (Client &c: clients) {
		if (c.ctx) {
			tls_close(c.ctx);
			tls_free(c.ctx);
		}
		if (c.fd >= 0)
			close(c.fd);
	}

	if (fd > 0)
		close(fd);
	if (ctx)
		tls_free(ctx);
}

void Server::freeClient(int idx) {
	Client &c = clients[idx];
	if (c.ctx) {
//		tls_close(c.ctx); // DoS potential?
		tls_free(c.ctx);
	}
	if (c.fd >= 0)
		close(c.fd);
	clients.erase(clients.begin() + idx);
}

void Server::httpParse(Client &c, int idx) {
	auto copyToUpper = [] (std::string &dst, std::string_view src, size_t start, size_t end) {
		std::string_view tmp(begin(src) + start, end - start);
		dst.clear();
		for (auto &c : tmp)
		dst.push_back(toupper(c));
	};
	std::string_view data(c.buffer.data(), c.buffer.size());

	if (c.request.parseState == HttpRequest::REQ_LINE) {
		size_t pos = data.find("\r\n");
		if (pos == std::string_view::npos)
			return;
		std::string_view req_line = data.substr(0, pos);
		size_t uriPos = req_line.find(' ');
		if (uriPos == std::string::npos) {
			c.request.parseState = HttpRequest::INVALID;
			return;
		}
		copyToUpper(c.request.method, req_line, 0, uriPos);

		uriPos ++;
		size_t verPos = req_line.find(' ', uriPos);
		if (verPos == std::string::npos) {
			c.request.parseState = HttpRequest::INVALID;
			return;
		}
		c.request.uri = std::string(begin(req_line) + uriPos, verPos - uriPos);

		copyToUpper(c.request.version, req_line, verPos + 1, pos);

		c.parsePos = pos + 2;
		c.request.parseState = HttpRequest::HEADERS;
	}
	if (c.request.parseState == HttpRequest::HEADERS) {
		size_t headerEnd = data.find("\r\n\r\n", c.parsePos);
		if ( headerEnd == std::string_view::npos) {
			return;
		}
		std::string_view header(begin(data) + c.parsePos, headerEnd - c.parsePos);

		ssize_t currPos = 0;
		while (currPos <= header.size()){
			ssize_t nextLn = header.find("\r\n", currPos);
			if (nextLn == std::string::npos)
				nextLn = header.size(); 
			std::string_view ln = (nextLn == std::string::npos) ?
				header.substr(currPos) :
				header.substr(currPos, nextLn - currPos);

			if (!ln.empty()) {
				size_t split = ln.find(':');
				if (split != std::string::npos) {
					std::string key;
					copyToUpper(key, ln, 0, split);
					split ++;
					if (split < ln.size() && ln[split] == ' ')
						split ++;

					c.request.headers[key] = std::string(begin(ln) + split,
							ln.size() - split);
				}
			}

			if (nextLn == std::string::npos)
				break;
			currPos = nextLn + 2;
		}

		c.parsePos = headerEnd + 4;
		c.request.parseState = HttpRequest::BODY;

		auto it = c.request.headers.find("CONTENT-LENGTH");
		c.request.contentLength = (it != c.request.headers.end()) ?
				std::stoul(it->second) : 0;

		if (c.request.contentLength == 0) {
			c.request.parseState = HttpRequest::COMPLETE;
		} else if (c.request.contentLength > maxReqSize) {
			lerr << "Client requesting " << c.request.contentLength << ". Dropping" << log::endl;
			c.request.parseState = HttpRequest::INVALID;
			return;
		}
	}

	if (c.request.parseState == HttpRequest::BODY) {
		if (c.buffer.size() - c.parsePos < c.request.contentLength) {
			return;
		}
		c.request.body.assign(c.buffer.data() + c.parsePos, c.request.contentLength);
		c.parsePos += c.request.contentLength;
		c.request.parseState = HttpRequest::COMPLETE;
	}
};

void Server::run() {
	std::vector<struct pollfd> pfds;
	fcntl(fd, F_SETFL, O_NONBLOCK);
	while (true) {
		pfds.clear();
		pfds.push_back({fd, POLLIN, 0}); // Listener
		pfds.push_back({wakePipe[0], POLLIN, 0});

		for (auto &c : clients) {
			short events = POLLERR | POLLHUP;
			if (c.writePending)
				events |= POLLOUT;
			else
				events |= POLLIN;
			pfds.push_back({c.fd, events, 0});
		}

		size_t polledClientsCount = clients.size();

		int nfds = poll(&pfds[0], pfds.size(), 1000);

		if (nfds < 0) {
			if (errno == EINTR)
				continue;
			lerr << "Poll error" << log::endl;
			break;
		}

		if (!nfds)
			continue;

		// Listener accepts client
		if (pfds[0].revents & POLLIN) {
			while (true) {
				int cFd = accept(fd, nullptr, nullptr);
				if (cFd < 0)
					break;

				fcntl(cFd, F_SETFL, O_NONBLOCK);

				struct tls* cCtx = nullptr;
				if (!tls_accept_socket(ctx, &cCtx, cFd)) {
					if (clients.size() < maxConnections) {
						linfo << "New client" << log::endl;
						clients.push_back({cCtx, cFd});
						clients.back().lastActivity =
							std::chrono::steady_clock::now();
					} else {
						if (cCtx)
							tls_free(cCtx);
						close(cFd);
					}
				} else {
					if (cCtx)
						tls_free(cCtx);
					close(cFd);
				}
			}
		}

		if (pfds[1].revents & POLLIN) {
			char dummy[256];
			while (read(wakePipe[0], dummy, sizeof(dummy)) > 0) {}

			HttpResponse res;
			while (global_send_queue.pop(res)) {
				for (int i = 0; i < clients.size(); i++) {
					auto &c = clients[i];
					if (c.fd == res.client_fd) {
						std::string data = res.serialize();
						linfo << "Sending Response" << log::endl;
						int r = tls_write(c.ctx, data.c_str(), data.size());
						if (r < 0) {
							lerr << "Write failed, cleaning up client" << log::endl;
							freeClient(i); 
						} else {
							c.reset();
						}
						break;
					}
				}
			}
		}

		for (int i = (int)polledClientsCount - 1; i >= 0; i--) {
			if (i >= (int)clients.size())
				continue;

			Client &c = clients[i];

			auto now = std::chrono::steady_clock::now();
			if (std::chrono::duration_cast<std::chrono::seconds>
					(now - c.lastActivity).count() > timeout) {
				linfo << "Connection timeout for client ";
				linfo << c.fd << "(" << i << ")" << log::endl;
				freeClient(i);
				continue;
			}
			if (c.fd < 0)
				continue;

			int revents = pfds[i + 2].revents;

			if (revents & (POLLERR | POLLHUP)) {
				freeClient(i);
				continue;
			}

			if (!c.handshake) {
				if (revents & (POLLIN | POLLOUT)) {
					int h = tls_handshake(c.ctx);
					if (h == 0) {
						c.handshake = true;
						c.writePending = false;
						c.lastActivity = now;
					} else if (h == TLS_WANT_POLLIN) {
						c.writePending = false;
					} else if (h == TLS_WANT_POLLOUT) {
						c.writePending = true;
					} else {
						lerr << "Handshake failed: ";
						lerr << tls_error(c.ctx) << log::endl;
						freeClient(i);
						continue;
					}
				}
				continue;
			}

			if (revents & POLLIN) {
				char readBuf[4096];
				int r = tls_read(c.ctx, readBuf, sizeof(readBuf));
				if (r == TLS_WANT_POLLIN || r == TLS_WANT_POLLOUT) {
					c.writePending = (r == TLS_WANT_POLLOUT);
					continue;
		  		} else if (r <= 0) {
					if (r < 0) {
						lerr << "TLS Read Error: ";
						lerr << tls_error(c.ctx) << log::endl;
					}
					freeClient(i);
				} else {
					c.state = Client::PARSING;
					c.buffer.insert(c.buffer.end(), readBuf, readBuf + r);
					httpParse(c, i);


					if (c.request.parseState == HttpRequest::INVALID) {
						freeClient(i);
					}

					if (c.request.parseState == HttpRequest::COMPLETE) {
						c.lastActivity = std::chrono::steady_clock::now();
						c.state = Client::PROCESSING;

						int fd = c.fd;
						HttpRequest req = std::move(c.request);

						pool.enqueue([fd, req = std::move(req)]() {
							lua_State* L = threadLua.L;
							lua_getglobal(L, "on_request");
							lua_pushinteger(L, fd);
							push_request_to_lua(L, req);

							if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
								const char* error_msg = lua_tostring(L, -1);
								lerr << "CRITICAL LUA ERROR: " << (error_msg ? error_msg : "Unknown") << log::endl;
								lua_pop(L, 1);

								// Fallback: Send a 500 error if Lua fails
								HttpResponse res;
								res.client_fd = fd;
								res.status_code = 500;
								res.body = "Internal Server Error (Lua)";
								global_send_queue.push(std::move(res));
								char wake = 1;
								(void)!write(wakePipe[1], &wake, 1);
							}
						});
					}
				}
			}
		}
	}
}
