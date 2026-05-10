#ifndef __CONFIG_H__
#define __CONFIG_H__

#include <string>
#include <unordered_map>
#include <fstream>
#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>

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

enum eLogLevel {
	INFO,
	WARN,
	ERROR,
	LOG_MAX,
};

class Logger {
	std::string logStr[LOG_MAX] = {
		[INFO] = "INFO",
		[WARN] = "WARN",
		[ERROR] = "ERROR",
	};

	bool toFile = false;
	eLogLevel lvl;
public:
	Logger() = default;
	Logger(eLogLevel lvl) : lvl(lvl) {
	}

	template <typename T>
	Logger &operator<<(const T& val) {
		std::cout << val;
            return *this;
        }
};

extern Logger logger;

#endif // __CONFIG_H__
