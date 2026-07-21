#ifndef __SERVER_H_
#define __SERVER_H_

#include <vector>
#include <string>

#include <misc.h>
#include <tls.h>

#include <worker.h>

struct HttpHeader {
	std::string name;
	std::string value;
};

struct HttpRequest {
	std::string method;
	std::string uri;
	std::string version;
	std::unordered_map<std::string, std::string> headers;
	size_t contentLength = 0;
	enum { REQ_LINE, HEADERS, BODY, COMPLETE, INVALID} parseState = REQ_LINE;
	std::string body;
	void clear() {
		method.clear();
		uri.clear();
		version.clear();
		headers.clear();
		parseState = REQ_LINE;
		contentLength = 0;
		body = {};
	};
};

struct Client {
	struct tls *ctx;
	int fd;
	bool handshake = false;
	bool writePending = false;
	std::vector<char> buffer;
	size_t parsePos = 0;

	std::chrono::steady_clock::time_point lastActivity;
	HttpRequest request;

	enum { IDLE, PARSING, PROCESSING, RESPONDING, CLEANUP} state = IDLE;
	void reset() {
		request.clear();
		parsePos = 0;
		buffer.clear();
		state = IDLE;
		writePending = false;
	}
};

struct HttpResponse {
	int client_fd;
	int status_code = 200;
	std::string body;
	std::vector<std::pair<std::string, std::string>> headers;

	std::string serialize() {
		std::string response = "HTTP/1.1 " + std::to_string(status_code) + " " + 
							   (status_code == 303 ? "See Other" : "OK") + "\r\n"; 

		bool hasContentType = false;
		for (auto &h : headers) {
			if (h.first == "Content-Type") hasContentType = true;
			response += h.first + ": " + h.second + "\r\n";
		}

		response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
		if (!hasContentType && !body.empty())
			response += "Content-Type: text/html\r\n";

		response += "\r\n";
		response += body;
		return response;
	}
};

class Server {
	struct tls *ctx;
	int fd;
	int port;
	int timeout;
	int maxConnections;
	int numThreads;
	int maxReqSize;
	std::string certPath;
	std::string keyPath;
	std::string serveFile;
	std::string sqlDbFile;
	std::string protocols;
	std::vector<Client> clients;
	WorkerPool pool;

	void httpParse(Client &c, int idx);
public:
	Server(Config &cfg);
	~Server();
	void run();
	void freeClient(int idx);
};

class SendQueue {
	std::queue<HttpResponse> q;
	std::mutex mtx;
public:
	void push(HttpResponse res) {
		std::lock_guard<std::mutex> lock(mtx);
		q.push(std::move(res));
	}
	bool pop(HttpResponse &res) {
		std::lock_guard<std::mutex> lock(mtx);
		if (q.empty())
			return false;
		res = std::move(q.front());
		q.pop();
		return true;
	}
};

extern SendQueue global_send_queue;
extern int wakePipe[2]; // [0] = read, [1] = write

#endif // __SERVER_H_
