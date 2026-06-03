#ifndef __CONFIG_H__
#define __CONFIG_H__

#include <string>
#include <unordered_map>
#include <fstream>
#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <ctime>

class Config {
	std::unordered_map<std::string, std::string> configs;
	std::string cfgPath;
	std::string baseDir;
	bool loaded = false;
public:
	bool load(std::string cfgPath) {
		this->cfgPath = cfgPath;
		baseDir = std::filesystem::path(cfgPath).parent_path().string();
		std::string ln, section = "global";
		std::ifstream ifs(cfgPath);
		size_t pos;
		if (!ifs.is_open())
			return false;

		while (std::getline(ifs, ln)) {
			auto trim = [](std::string &str) {
				str.erase(std::remove(str.begin(), str.end(), ' '), str.end());
				str.erase(std::remove(str.begin(), str.end(), '\t'), str.end());
				str.erase(std::remove(str.begin(), str.end(), '\n'), str.end());
				str.erase(std::remove(str.begin(), str.end(), '\r'), str.end());
			};

			trim(ln);

			if (ln.empty() || ln[0] == '#')
				continue;

			if (ln[0] == '[' && ln.back() == ']') {
				section = ln;
				continue;
			}

			if ((pos = ln.find('=')) != std::string::npos) {
				std::string key = ln.substr(0, pos);
				std::string val = ln.substr(pos + 1);
				std::string fullKey = section + "." + key;
				configs[fullKey] = val;
			}
		}
		return true;
	}
	std::string getConfig(std::string section, std::string config, std::string def) {
		if (!section.empty() && section[0] != '[' && section.back() != ']')
			section = "[" + section + "]";
		std::string key = section + "." + config;
		auto it = configs.find(key);
		return it != configs.end() ? it->second : def;
	}
	uint32_t getConfigU32(std::string section, std::string config, uint32_t def) {
		std::string val = getConfig(section, config, "");
		uint32_t ret;
		try {
			ret = std::stoull(val);
		} catch (std::exception &e) {
			return def;
		}
		return val.empty() ? def : ret;
	}
	std::string getConfigFS(std::string section, std::string config, std::string def) {
		std::string val = getConfig(section, config, "");
		if (val.empty())
			return def;

		if (std::filesystem::path(val).is_absolute())
			return val;
		return baseDir + "/" + val;
	}
};

extern Config globalCfg;



namespace log {
	inline std::string getTimestamp() {
		using namespace std::chrono;
		auto now = system_clock::now();
		auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
		auto timer = system_clock::to_time_t(now);
		std::tm bt{};
		localtime_r(&timer, &bt);
		std::ostringstream oss;
		oss << "[" << std::put_time(&bt, "%Y-%m-%d %H:%M:%S");
		oss << '.' << std::setfill('0') << std::setw(3) << ms.count() << "] ";
		return oss.str();
	}

	struct EndLine {};
	const EndLine endl;

	enum eLogLevel {
		DEBUG,
		INFO,
		WARN,
		ERROR,
		LOG_MAX,
	};

	class Logger {
		std::string logStr[LOG_MAX] = {
			[DEBUG] = "DEBUG",
			[INFO] = "INFO",
			[WARN] = "WARN",
			[ERROR] = "ERROR",
		};

		bool toFile = false;
		eLogLevel lvl;
		bool isNewLine = true;
	public:
		Logger() : lvl(INFO), isNewLine(true) {}
		Logger(eLogLevel lvl) : lvl(lvl), isNewLine(true) {}

		template <typename T>
		Logger &operator<<(const T& val) {
			if (isNewLine) {
				std::cout << log::getTimestamp() << "[" << logStr[lvl] << "] ";
				isNewLine = false;
			}
			std::cout << val;

			return *this;
		}
		Logger &operator<<(const EndLine&) {
			std::cout << "\n" << std::flush;
			isNewLine = true;
			return *this;
		}
	};
}

extern log::Logger linfo;
extern log::Logger lwarn;
extern log::Logger lerr;
extern log::Logger ldbg;


#endif // __CONFIG_H__
