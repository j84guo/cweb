#include <stdio.h> // standard io like printf()
#include <stdlib.h> // miscellaneous standard functions, like memory allocation
#include <unistd.h> // posic types/functions like fork()
#include <errno.h> // errno integer is set by system calls
#include <string.h> // c string functions like strlen()
#include <fcntl.h> // operations on a file descriptor, like open()
#include <signal.h> // ansi c signal handling
#include <sys/types.h> // some basic types
#include <sys/socket.h> // socket types like sockaddr
#include <netinet/in.h> // network types like in_addr
#include <arpa/inet.h> // network functions like htons()
#include <pthread.h> // add multi-threading

#define VERSION 1
#define BUFSIZE 65536
#define ERROR      42
#define LOG        44
#define FORBIDDEN 403
#define NOTFOUND  404

#ifndef SIGCLD
#define SIGCLD SIGCHLD
#endif

// array of file structs mapping extension to HTTP Content-Type
struct {
	char *ext;
	char *filetype;
} extensions [] = { {"gif", "image/gif" },  {"jpg", "image/jpg" }, {"jpeg","image/jpeg"},
	{"png", "image/png" },  {"ico", "image/ico" },  {"zip", "image/zip" },
	{"gz",  "image/gz"  },  {"tar", "image/tar" },  {"htm", "text/html" },
	{"html","text/html" },  {0,0} };

// logs to cweb.log and may return HTTP error messages
void logger(int type, char *s1, char *s2, int socket_fd)
{
	int fd ; // log file descriptor
	char logbuffer[BUFSIZE*2]; // buffered i/o

	// notice sprintf output is ignored by casting to void
	switch (type) { // log level
	case ERROR: (void)sprintf(logbuffer,"ERROR: %s:%s Errno=%d exiting pid=%d",s1, s2, errno, getpid());
		break;
	case FORBIDDEN:
		(void)write(socket_fd, "HTTP/1.1 403 Forbidden\nContent-Length: 185\nConnection: close\nContent-Type: text/html\n\n<html><head>\n<title>403 Forbidden</title>\n</head><body>\n<h1>Forbidden</h1>\nThe requested URL, file type or operation is not allowed on this simple static file webserver.\n</body></html>\n",271);
		(void)sprintf(logbuffer,"FORBIDDEN: %s:%s",s1, s2);
		break;
	case NOTFOUND:
		(void)write(socket_fd, "HTTP/1.1 404 Not Found\nContent-Length: 136\nConnection: close\nContent-Type: text/html\n\n<html><head>\n<title>404 Not Found</title>\n</head><body>\n<h1>Not Found</h1>\nThe requested URL was not found on this server.\n</body></html>\n",224);
		(void)sprintf(logbuffer,"NOT FOUND: %s:%s",s1, s2);
		break;
	case LOG: (void)sprintf(logbuffer," INFO: %s:%s:%d",s1, s2,socket_fd); break;
	}

	if((fd = open("cweb.log", O_CREAT| O_WRONLY | O_APPEND, 0644)) >= 0) {
		(void)write(fd, logbuffer, strlen(logbuffer));
		(void)write(fd, "\n", 1);
		(void)close(fd);
	}

	// exit child process on error
	if(type == ERROR || type == NOTFOUND || type == FORBIDDEN){
		exit(3);
	}
}

// new thread
void* web(void* fdv)
{
	int* fd = (int*) fdv;

	int file_fd;
	int buflen;

	long i;
	long ret;
	long len;

	char * fstr;

	static char buffer[BUFSIZE+1]; // static variables are zero filled

	ret = read(fd, buffer, BUFSIZE);
	if(ret == 0 || ret == -1) {
		logger(FORBIDDEN,"failed to read browser request","",fd);
	}

	if(ret > 0 && ret < BUFSIZE) {
		buffer[ret] = 0; // terminate buffer
	} else {
		buffer[0] = 0;
	}

	for(i=0; i<ret; i++) {
		// one line of log
		if(buffer[i] == '\r' || buffer[i] == '\n') {
			buffer[i]='*';
		}
	}

	logger(LOG, "request", buffer, 1);

	if(strncmp(buffer, "GET ", 4) && strncmp(buffer,"get ", 4)) {
		logger(FORBIDDEN,"Only simple GET operation supported",buffer,fd);
	}

	for(i=4; i<BUFSIZE; i++) { // terminate after second space (after path)
		if(buffer[i] == ' ') {
			buffer[i] = 0;
			break;
		}
	}

	for(int j=0; j<i-1; j++){	// check for illegal parent directory use
		if(buffer[j] == '.' && buffer[j+1] == '.') {
			logger(FORBIDDEN,"Parent directory (..) path names not supported",buffer,fd);
		}
	}

	if(!strncmp(&buffer[0],"GET /\0",6) || !strncmp(&buffer[0],"get /\0",6) ) { // no filename defaults to index file
		(void)strcpy(buffer,"GET /index.html");
	}

  // find Content-Type
	buflen = strlen(buffer);
	fstr = (char *) 0;
	for(i=0; extensions[i].ext != 0; i++) {
		len = strlen(extensions[i].ext);
		if(!strncmp(&buffer[buflen-len], extensions[i].ext, len)) {
			fstr = extensions[i].filetype;
			break;
		}
	}

	if(fstr == 0) logger(FORBIDDEN,"file extension type not supported",buffer,fd); // unsupported file extension

	if((file_fd = open(&buffer[5], O_RDONLY)) == -1) {  // open the file for reading
		logger(NOTFOUND, "failed to open file", &buffer[5], fd);
	}

	// HTTP response
	logger(LOG, "SEND", &buffer[5], 1);
	len = (long)lseek(file_fd, (off_t)0, SEEK_END); // lseek to the file end to find the length
	      (void)lseek(file_fd, (off_t)0, SEEK_SET); // lseek back to the file start ready for reading
        (void)sprintf(buffer, "HTTP/1.1 200 OK\nServer: cweb/%d.0\nContent-Length: %ld\nConnection: close\nContent-Type: %s\n\n", VERSION, len, fstr);
	logger(LOG, "Header", buffer, 1);
	(void)write(fd,buffer,strlen(buffer));

	// send file in 8KB block - last block may be smaller
	while ((ret = read(file_fd, buffer, BUFSIZE)) > 0) {
		(void)write(fd,buffer,ret);
	}
	sleep(1);	// allow socket to drain before signalling the socket is closed
	close(fd);
}

int main(int argc, char **argv)
{
	int i;
	int port;
	int pid;
	int listenfd;
	int socketfd;
	int hit;

	socklen_t length;
	static struct sockaddr_in cli_addr; // static = initialised to zeros
	static struct sockaddr_in serv_addr; // static = initialised to zeros

  // usage message
	if(argc < 3  || argc > 3 || !strcmp(argv[1], "-?")) {
		(void)printf("hint: cweb port-number root-directory\t\tversion %d\n\n"
	"\tcweb only servers out file/web pages with extensions named below\n"
	"\t and only from the named directory or its sub-directories.\n"
	"\tE.g.: cweb 8080 /home/jackson/cwebd &\n\n"
	"\tOnly Supports:", VERSION);
		for(i=0;extensions[i].ext != 0;i++){
			(void)printf(" %s",extensions[i].ext);
		}
		(void)printf("\n\tNot Supported: URLs including \"..\", dynamic content\n"
	"\tNot Supported: directories / /etc /bin /lib /tmp /usr /dev /sbin \n");
		exit(0);
	}

	// forbidden directories
	if( !strncmp(argv[2],"/"   ,2 ) ||
			!strncmp(argv[2],"/etc", 5 ) ||
	    !strncmp(argv[2],"/bin",5 ) ||
			!strncmp(argv[2],"/lib", 5 ) ||
	    !strncmp(argv[2],"/tmp",5 ) ||
			!strncmp(argv[2],"/usr", 5 ) ||
	    !strncmp(argv[2],"/dev",5 ) ||
			!strncmp(argv[2],"/sbin",6) ) {
		(void)printf("ERROR: Bad top directory %s, see usage with cweb -?\n",argv[2]);
		exit(3);
	}

	// enter root directory
	if(chroot(argv[2]) == -1){
		(void)printf("ERROR: Can't Change to directory %s\n",argv[2]);
		exit(4);
	}

	// become daemon (no zombie child processes)
	if(fork() != 0){
		return 0; // parent returns OK to shell
	}

	(void)signal(SIGCLD, SIG_IGN); // ignore child death
	(void)signal(SIGHUP, SIG_IGN); // ignore terminal hangups

	for(i=0;i<32;i++) {
		(void)close(i);	// close open files
	}

	(void)setpgrp();		// break away from process group

	logger(LOG,"cweb starting",argv[1],getpid());

	// get server socket descriptor
	if((listenfd = socket(AF_INET, SOCK_STREAM,0)) <0) {
		logger(ERROR, "system call","socket",0);
	}

	// parse port
	port = atoi(argv[1]);
	if(port < 0 || port >60000) {
		logger(ERROR,"Invalid port number (try 1->60000)",argv[1],0);
	}

	// server socket struct
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(port);

	// bind server socket to port
	if(bind(listenfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) <0){
		logger(ERROR,"system call","bind",0);
	}

	// listen for TCP connections
	if( listen(listenfd,64) <0) {
		logger(ERROR,"system call","listen",0);
	}

  // number requests
	for(hit=1; ;hit++) {
		length = sizeof(cli_addr);

		if((socketfd = accept(listenfd, (struct sockaddr *) &cli_addr, &length)) < 0) {
			logger(ERROR,"system call","accept",0);
		}

		// thread per request model
		pthread_t handler_thread; // new thread
		if(pthread_create(&handler_thread, NULL, web, (void*) socketfd) < 0){
			logger(ERROR,"system call","pthread_create",0);
			return 1;
		}
		(void) close(socketfd);
	}
}
