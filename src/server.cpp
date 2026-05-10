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

	std::cout << "Launching server on port " << port << std::endl;
	std::cout << "Keys: " << std::endl;
	std::cout << "\t" << certPath << std::endl;
	std::cout << "\t" << keyPath << std::endl;
	std::cout << "Security option: " << protocols << std::endl;
	std::cout << "Max connection count: " << maxConnections << std::endl;
	std::cout << "Worker pool size: " << numThreads << std::endl;
	std::cout << "Max request size: " << maxReqSize << std::endl;
	std::cout << "Serving file: " << serveFile << std::endl;

	if (keyPath.empty() || certPath.empty()) {
		std::cerr << "Failed to find ssl keys" << std::endl;
		exit(-1);
	}


	tls_init();
	config = tls_config_new();
	tls_config_parse_protocols(&protocolsCfg, protocols.c_str());
	tls_config_set_protocols(config, protocolsCfg);

	if (tls_config_set_cert_file(config, certPath.data()) != 0 ||
			tls_config_set_key_file(config, keyPath.data()) != 0) {
		std::cerr << "Cert/Key error: " << tls_config_error(config) << std::endl;
		exit(-1);
	}

	ctx = tls_server();
	tls_configure(ctx, config);
	tls_config_free(config);

	fd = socket(AF_INET, SOCK_STREAM, 0);
	int opt = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;
	bind(fd, (struct sockaddr*)&addr, sizeof(addr));
	listen(fd, SOMAXCONN);

	if (pipe(wakePipe) == -1) {
		std::cerr << "Failed to open pipe" << std::endl;
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
			std::cerr << "Client requesting " << c.request.contentLength << ". Dropping" << std::endl;
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
	while (true) {
		pfds.clear();
		pfds.push_back({fd, POLLIN, 0}); // Listener

		pfds.push_back({wakePipe[0], POLLIN, 0});

		for (auto &c : clients)
			pfds.push_back({c.fd, POLLIN | POLLERR, 0});

		int nfds = poll(&pfds[0], pfds.size(), -1);
		if (nfds < 0) {
			if (errno == EINTR)
				continue;
			std::cerr << "Poll error" << std::endl;
			break;
		}

		if (!nfds)
			continue;

		// Listener accepts client
		if (pfds[0].revents & POLLIN) {
			int cFd = accept(fd, nullptr, nullptr);
			struct tls* cCtx;
			if (!tls_accept_socket(ctx, &cCtx, cFd)) {
				if (clients.size() <= maxConnections) {
					std::cout << "New client" << std::endl;
					clients.push_back({cCtx, cFd});

					clients.back().lastActivity = std::chrono::steady_clock::now();
				} else {
					tls_free(cCtx);
					close(cFd);
				}
			} else {
				tls_free(cCtx);
				close(cFd);
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
						std::cout << "Sending Response" << std::endl;
						int r = tls_write(c.ctx, data.c_str(), data.size());
						if (r < 0) {
							std::cerr << "Write failed, cleaning up client" << std::endl;
							freeClient(i); 
						} else {
							c.reset();
						}
						break;
					}
				}
			}
		}

		for (int i = clients.size() - 1; i >= 0; i--) {
			Client &c = clients[i];
			if (c.fd < 0)
				continue;

			if (!c.handshake) {
				int h = tls_handshake(c.ctx);
				if (h == 0) {
					c.handshake = true;
					continue;
				} else if (h == TLS_WANT_POLLIN || h == TLS_WANT_POLLOUT) {
					continue;
				} else {
					std::cerr << "Handshake failed: " << tls_error(c.ctx) << std::endl;
					freeClient(i);
					continue;
				}
			}

			int revents = pfds[i + 2].revents;

			if (revents & (POLLERR | POLLHUP)) {
				freeClient(i);
				continue;
			}

			if (revents & POLLIN) {
				char readBuf[4096];
				int r = tls_read(c.ctx, readBuf, sizeof(readBuf));
				if (r == TLS_WANT_POLLIN || r == TLS_WANT_POLLOUT) {
					continue;
		  		} else if (r < 0) {
					std::cerr << "TLS Read Error: " << tls_error(c.ctx) << std::endl;
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
								std::cerr << "CRITICAL LUA ERROR: " << (error_msg ? error_msg : "Unknown") << std::endl;
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
