#include <sysexits.h>

#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <pthread.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <time.h>

#include "logger.hpp"
#include "HttpdServer.hpp"

#define MAXPENDING 10
#define MAXDATASIZE 1000

map<string,string> g_mime;
string root_dir;

struct ARG {  
    int connfd;  
    struct sockaddr_in client;  
}; 
 
void httpClientHandler(int connfd, struct sockaddr_in client);

int isInitialLine(string msg);

string initialLineHandler(string line, int* errorCode);

int headerHandler(string header, int* errorCode);

string responseBuilder(string url, bool host_exist, int* errorCode);

void sendResponse(int connfd, string responseStr, int errorCode, string url);

string getAbsolutePath(string path){
        char resolved_path[100];
        realpath(path.c_str(), resolved_path);
        string res(resolved_path);
        return res;
}

void *pfunc(void * argument){
        struct ARG *info;
        info = (struct ARG*) argument;
        httpClientHandler(info->connfd, info->client);
        free(argument);
        pthread_exit(NULL);
}

HttpdServer::HttpdServer(INIReader& t_config)
	: config(t_config)
{
	auto log = logger();

	string pstr = config.Get("httpd", "port", "");
	if (pstr == "") {
		log->error("port was not in the config file");
		exit(EX_CONFIG);
	}
	port = pstr;

	string dr = config.Get("httpd", "doc_root", "");
	if (dr == "") {
		log->error("doc_root was not in the config file");
		exit(EX_CONFIG);
	}
	doc_root = dr;
	
	string mmr = config.Get("httpd", "mime_types", "");
	if (mmr == ""){
		log->error("mime_types was not in the config file");
		exit(EX_CONFIG);
	}
	mime_path = mmr;
	readMIME();
}

void HttpdServer::readMIME(){
	fstream in(mime_path.c_str());
        string line;
        if(in) {
                while (getline (in, line)) {
                        size_t spacePos = line.find(" ");
                        string key = line.substr(0, spacePos);
                        string value = line.substr(spacePos + 1);
                        mime.insert(pair<string, string>(key, value));
                }
        } else {
                cout <<"no such file" << endl;
        }
			
}

void HttpdServer::launch()
{
	auto log = logger();

	log->info("Launching web server");
	log->info("Port: {}", port);
	log->info("doc_root: {}", doc_root);

	// Put code here that actually launches your webserver...
        int servSock;
	int clntSock;
	struct sockaddr_in httpServAddr;
	struct sockaddr_in httpClntAddr;
	unsigned short httpServPort;
	unsigned int clntLen;
	pthread_t th;
	struct ARG *argument;

	g_mime = mime;
	root_dir = getAbsolutePath(doc_root);
 	httpServPort = atoi(port.c_str());	

	if ((servSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
		perror("socket() fail");
	
	memset(&httpServAddr, 0, sizeof(httpServAddr));
	httpServAddr.sin_family = AF_INET;
	httpServAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	httpServAddr.sin_port = htons(httpServPort);

	if (bind(servSock, (struct sockaddr *) &httpServAddr, sizeof(httpServAddr)) < 0)
		perror("bind() fail");

	if (listen(servSock, MAXPENDING) < 0)
		perror("listen() fail");

	while(true){
		clntLen = sizeof(httpClntAddr);
		if ((clntSock = accept(servSock, (struct sockaddr *) &httpClntAddr, &clntLen)) < 0)
			perror("accpet() fail");
		
		argument = (struct ARG *)malloc(sizeof(struct ARG));
		argument->connfd = clntSock;
		memcpy((void*)&argument->client, &httpClntAddr, clntLen);
	
		if(pthread_create(&th, NULL, pfunc, (void*)argument)){
			perror("Pthread_create() fail");
			exit(1);
		}
	}
	close(clntSock);
	close(servSock);	
			
}

void httpClientHandler(int connfd, struct sockaddr_in client){
	int recvLen;
	string msgbuf;
	char recvbuf[MAXDATASIZE];
  	auto log = logger();
	int errorCode = 200;
	bool host_exist = false;
	bool close_signal = false;
	bool inProgress = false;
	bool isEnd = false;
	string reqURL;
	
	struct timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
                
        if (setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0) {
        	perror("setsockopt()");
        }
	
	log->info("Got a connection from {} ",inet_ntoa(client.sin_addr));
	while(true){
		memset(recvbuf, 0, MAXDATASIZE);
		
		recvLen = recv(connfd, recvbuf, MAXDATASIZE, 0);
		
		if(recvLen < 0){
			if (errno == EAGAIN || errno == EWOULDBLOCK){
				fprintf(stderr, "Client timed out, closing connection\n");
				close(connfd);
				return;
			}
			perror("recv()");
			close(connfd);
			return;
		} 	

		if(recvLen == 0){
			fprintf(stderr, "remote end close the connection\n");
			close(connfd);
			return;
		}
		
		msgbuf = msgbuf + string(recvbuf, recvLen);
		//cout << "Message buffer is : \n" << msgbuf.c_str() << endl;
		size_t delimoffset = msgbuf.find("\r\n");
		while(delimoffset != string::npos){
			string msg = msgbuf.substr(0, delimoffset);
			msgbuf = msgbuf.substr(delimoffset+2);
			delimoffset = msgbuf.find("\r\n");
			if (msg.length() == 0) {
				isEnd = true;
				continue;
			}
			isEnd = false;	
			if (isInitialLine(msg) == 0){
				if(inProgress){
					//send();
					string responseStr = responseBuilder(reqURL, host_exist, &errorCode);
					sendResponse(connfd, responseStr, errorCode, reqURL);
					inProgress = false;
				}
				if (close_signal == true || errorCode == 400){
					close(connfd);
					return;
				}
                                host_exist = false;
                                close_signal = false;
                                errorCode = 200;

				inProgress = true;
				//handle initial line;
				reqURL = initialLineHandler(msg, &errorCode);
			} else {
				//handle headers
				int r = headerHandler(msg, &errorCode);
				if (r == -1) close_signal = true;
				if (r == 1) host_exist = true;
				//cout << msg << ":" << host_exist << endl;
			}
		}

		if (isEnd){
                	string responseStr = responseBuilder(reqURL, host_exist, &errorCode);
                        sendResponse(connfd, responseStr, errorCode, reqURL);
                        inProgress = false;
            
            		if (close_signal == true || errorCode == 400){
                        	close(connfd);
                        	return;
                	}
		}
	}
}

void sendResponse(int connfd, string responseStr, int errorCode, string url){
	int length = strlen(responseStr.c_str());
	int sent_len;
	while((sent_len = send(connfd, responseStr.c_str(), length, 0)) != length){
		if (sent_len < 0){
			cout << "send() fail" << endl;
			break;
		}
		responseStr=responseStr.substr(sent_len);
		length = strlen(responseStr.c_str());
	}
	if (url.compare("/") == 0) url += "index.html";
	string filePath;
	if (errorCode == 200) filePath = (root_dir + url);
	else if (errorCode == 404) filePath = (root_dir + "/404.html");
	else filePath = (root_dir + "/400.html");
	struct stat stat_buf;
	stat(filePath.c_str(), &stat_buf);
	if (S_ISDIR(stat_buf.st_mode) && errorCode == 200) {
        	filePath += "/index.html";
		stat(filePath.c_str(), &stat_buf);
        }
	int filefd = open(filePath.c_str(), O_RDONLY);
	if(sendfile(connfd, filefd, NULL, stat_buf.st_size) < 0)
		perror("sendfile() fail");
		
}

int isInitialLine(string msg){
	size_t getPos = msg.find("GET");
	if (getPos == string::npos) return -1;
	size_t httpPos = msg.find("HTTP/1.1");
	if (httpPos == string::npos) return -1;
	return 0;
}

string initialLineHandler(string line, int* errorCode){
	size_t getPos = line.find("GET ");
	size_t httpPos = line.rfind(" HTTP/1.1");
	size_t slashPos = line.find("/");
	if (getPos != 0 || httpPos == string::npos || slashPos != getPos + 4) {
		*errorCode = 400;
		cout << "initialLine: " << getPos << "," << httpPos << endl;
		return string("null");
	}
	string url = line.substr(getPos + 4, httpPos - getPos - 4);
	return url;
}

int headerHandler(string header, int* errorCode){
	size_t csPos = header.find(": ");
	if (csPos == string::npos || csPos == 0){
		cout << "header: " << csPos << endl;
		*errorCode = 400;
	}
	string key = header.substr(0, csPos);
	string value = header.substr(csPos + 2);
	if ((key.compare("Connection") == 0) && (value.compare("close") == 0)) return -1;
	if (key.compare("Host") == 0) return 1;		
	return 0;
}

string responseBuilder(string url, bool host_exist, int* errorCode){
	string header_server("Server: MyHttpServer\r\n");
	string header_end("\r\n");
	string message;
	//cout << "host: " << host_exist << endl;
	if(!host_exist) *errorCode = 400;
	//cout << "host: " << host_exist << endl;
	if (*errorCode == 400){
		string nfpage(root_dir + "/400.html");
                struct stat s_buf;
                stat(nfpage.c_str(), &s_buf);
                int content_size = s_buf.st_size;
		
		message+=string("HTTP/1.1 400 Client Error\r\n");
		message+=header_server;
		message+=string("Content-Length: " + to_string(content_size) + "\r\n");
		message+=("Content-Length: text/html\r\n");
		message+=header_end;
	} else {
		if (url.compare("/") == 0) url += "index.html";
		string filePath(root_dir + url);
		//ifstream in(filePath.c_str());
		struct stat s_buf;
		stat(filePath.c_str(), &s_buf);
		if (S_ISDIR(s_buf.st_mode)) {
			filePath += "/index.html";
			stat(filePath.c_str(), &s_buf);
		}
		filePath = getAbsolutePath(filePath);

                size_t pathCheck = filePath.find(root_dir);

		if (S_ISREG(s_buf.st_mode) && pathCheck != string::npos){
        	        int content_size = s_buf.st_size;

			int pos = filePath.rfind(".");
			string post = filePath.substr(pos);
			string header_type("Content-Type: ");
			if (g_mime.count(post) > 0)
				header_type += g_mime[post] + "\r\n";
			else header_type += "application/octet-stream\r\n";

			time_t rawtime;
			struct tm *timeinfo;
			char tbuff[80];
			time(&rawtime);
			timeinfo=localtime(&rawtime);
			strftime(tbuff, 80, "%a, %d %b %y %T %z", timeinfo);
			message+=string("HTTP/1.1 200 OK\r\n");
			message+=header_server;
			message+=string("Last-Modified: " + string(tbuff) + "\r\n");
			message+=string("Content-Length: " + to_string(content_size) + "\r\n");
			message+=header_type;
			message+=header_end;
		} else *errorCode = 404;

	}
	if (*errorCode == 404){
		string nfpage(root_dir + "/404.html");
		struct stat s_buf;
		stat(nfpage.c_str(), &s_buf);
		int content_size = s_buf.st_size;
		message+=string("HTTP/1.1 404 Not Found\r\n");
		message+=header_server;
		message+=string("Content-Length: " + to_string(content_size) + "\r\n");
		message+=string("Content-type: text/html\r\n");
		message+=header_end;
	}	 					
	return message;	
}	
