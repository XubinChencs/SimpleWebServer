#ifndef HTTPDSERVER_HPP
#define HTTPDSERVER_HPP

#include "inih/INIReader.h"
#include "logger.hpp"
#include <map>

using namespace std;

class HttpdServer {
public:
	HttpdServer(INIReader& t_config);

	void launch();

protected:
	INIReader& config;
	string port;
	string doc_root;
	string mime_path;
	map<string, string> mime;
	
	void readMIME();
};

#endif // HTTPDSERVER_HPP
