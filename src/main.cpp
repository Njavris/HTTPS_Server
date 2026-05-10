#include <iostream>
#include <string>

#include <getopt.h>

#include <misc.h>
#include <server.h>

Logger logger(INFO);

Config globalCfg;

int main(int argn, char **argc) {
	signal(SIGPIPE, SIG_IGN);
	std::string cfgPath = "./cfg.ini";

	int opt;
	while ((opt = getopt(argn, argc, "C:")) != -1) {
		switch (opt) {
		case 'C':
			cfgPath = std::string(optarg);	
			break;
		default:
			break;
		}
	}
	globalCfg.load(cfgPath);
	
	Server server(globalCfg);
	server.run();

	return 0;
}
