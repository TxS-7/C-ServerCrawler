#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h> // isalnum
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h> // select
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <arpa/inet.h> // htonl, htons
#include <pthread.h>
#include <signal.h>
#include <sys/time.h> // gettimeofday
#include <errno.h>
#include "req_queue.h"
#include "requests.h"

#define BUF_SIZE 256

#define CMD_OK       0
#define CMD_SHUTDOWN 1
#define CMD_INVALID -1

static void *threadFunc(void *);
static void serveClient(char *, int);
static int invalidFile(char *);
static int handleCommand(int, long long);
static int handleRequest(int, char *);
static void cleanup(pthread_t *, int);
static void usage(char *);


// Used to stop threads when shutting down server
static int threadStop = 0;
static pthread_mutex_t thread_stop_mtx = PTHREAD_MUTEX_INITIALIZER;

// Variables used for STATS command
static int pagesServed = 0;
static int bytesServed = 0;
static pthread_mutex_t stats_mtx = PTHREAD_MUTEX_INITIALIZER;

// Queue of requests to be served by the threads
static RequestQueue reqQueue;
static pthread_mutex_t queue_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_nonempty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t cond_nonfull = PTHREAD_COND_INITIALIZER;


int main(int argc, char *argv[]) {
	if (argc != 9) {
		usage(argv[0]);
		return -1;
	}

	// Ignore SIGPIPEs. We will handle the errors
	signal(SIGPIPE, SIG_IGN);

	int sport;
	int cport;
	int threadCount;
	char *dirname;
	struct stat dirStat;

	// Parse arguments
	int got_sport = 0;
	int got_cport = 0;
	int got_threads = 0;
	int got_dir = 0;
	int i;
	for (i = 1; i < argc; i += 2) {
		if (strcmp(argv[i], "-p") == 0 && !got_sport) {
			got_sport = 1;
			sport = atoi(argv[i+1]);
			if (sport <= 0 || sport > 65535) {
				fprintf(stderr, "[-] Port number must be between 1 and 65535\n");
				return -1;
			}
		} else if (strcmp(argv[i], "-c") == 0 && !got_cport) {
			got_cport = 1;
			cport = atoi(argv[i+1]);
			if (cport <= 0 || cport > 65535) {
				fprintf(stderr, "[-] Port number must be between 1 and 65535\n");
				return -1;
			}
		} else if (strcmp(argv[i], "-t") == 0 && !got_threads) {
			got_threads = 1;
			threadCount = atoi(argv[i+1]);
			if (threadCount <= 0) {
				fprintf(stderr, "[-] The number of threads must be a positive integer\n");
				return -1;
			}
		} else if (strcmp(argv[i], "-d") == 0 && !got_dir) {
			got_dir = 1;
			dirname = argv[i+1];

			// Check if directory exists and is readable and executable
			if (access(dirname, F_OK | R_OK | X_OK) == -1) {
				fprintf(stderr, "[-] Invalid directory %s\n", dirname);
				return -1;
			}

			// Check if the name given is a directory
			if (stat(dirname, &dirStat) != 0) {
				perror("stat");
				return -1;
			}
			if (!S_ISDIR(dirStat.st_mode)) {
				fprintf(stderr, "[-] Invalid directory %s\n", dirname);
				return -1;
			}
		} else {
			usage(argv[0]);
			return -1;
		}
	}



	// Get the start time in milliseconds
	struct timeval tv;
	gettimeofday(&tv, NULL);
	long long startTime = tv.tv_sec * 1000 + tv.tv_usec / 1000;

	queueInit(&reqQueue);


	// Create the thread pool
	pthread_t *threads = malloc(threadCount * sizeof(pthread_t));
	for (i = 0; i < threadCount; i++) {
		pthread_create(&threads[i], NULL, threadFunc, NULL);
	}


	int reuse = 1;

	// WEB SOCKET
	int web_sock;
	if ((web_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("socket: web");
		cleanup(threads, threadCount);
		return -2;
	}
	// Avoid TIME_WAIT state
	if (setsockopt(web_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
		perror("setsockopt: web");
		cleanup(threads, threadCount);
		close(web_sock);
		return -2;
	}

	struct sockaddr_in web_server;
	web_server.sin_family = AF_INET;
	web_server.sin_addr.s_addr = htonl(INADDR_ANY);
	web_server.sin_port = htons(sport);
	if (bind(web_sock, (struct sockaddr *) &web_server, sizeof(web_server)) < 0) {
		perror("bind: web");
		cleanup(threads, threadCount);
		close(web_sock);
		return -2;
	}

	// Set the socket as non-blocking to handle client disconnection after select succeeds
	int opts = fcntl(web_sock, F_GETFL);
	if (opts < 0) {
		perror("fcntl: F_GETFL");
		cleanup(threads, threadCount);
		close(web_sock);
		return -2;
	}
	if (fcntl(web_sock, F_SETFL, opts | O_NONBLOCK) < 0) {
		perror("fcntl: F_SETFL");
		cleanup(threads, threadCount);
		close(web_sock);
		return -2;
	}

	if (listen(web_sock, 16) < 0) {
		perror("listen: web");
		cleanup(threads, threadCount);
		close(web_sock);
		return -2;
	}
	printf("[+] Listening for requests on port %d\n", sport);


	// COMMAND SOCKET
	int cmd_sock;
	if ((cmd_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("socket: cmd");
		cleanup(threads, threadCount);
		close(web_sock);
		return -2;
	}
	// Avoid TIME_WAIT state
	if (setsockopt(cmd_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
		perror("setsockopt: command");
		cleanup(threads, threadCount);
		close(web_sock);
		close(cmd_sock);
		return -2;
	}

	struct sockaddr_in cmd_server;
	cmd_server.sin_family = AF_INET;
	cmd_server.sin_addr.s_addr = htonl(INADDR_ANY);
	cmd_server.sin_port = htons(cport);
	if (bind(cmd_sock, (struct sockaddr *) &cmd_server, sizeof(cmd_server)) < 0) {
		perror("bind: command");
		cleanup(threads, threadCount);
		close(web_sock);
		close(cmd_sock);
		return -2;
	}

	// Set the socket as non-blocking to handle client disconnection after select succeeds
	opts = fcntl(cmd_sock, F_GETFL);
	if (opts < 0) {
		perror("fcntl: F_GETFL");
		cleanup(threads, threadCount);
		close(web_sock);
		close(cmd_sock);
		return -2;
	}
	if (fcntl(cmd_sock, F_SETFL, opts | O_NONBLOCK) < 0) {
		perror("fcntl: F_SETFL");
		cleanup(threads, threadCount);
		close(web_sock);
		close(cmd_sock);
		return -2;
	}

	if (listen(cmd_sock, 5) < 0) {
		perror("listen: command");
		cleanup(threads, threadCount);
		close(web_sock);
		close(cmd_sock);
		return -2;
	}
	printf("[+] Listening for commands on port %d\n\n", cport);



	int client_sock;
	struct sockaddr_in client;
	socklen_t client_len = sizeof(client);

	while (1) {
		// Check if a thread was terminated and we need to create a new one
		int j;
		for (j = 0; j < threadCount; j++) {
			// On success (0), the thread has been terminated
			if (pthread_tryjoin_np(threads[j], NULL) == 0) {
				printf("[-] A thread has been terminated\n");
				printf("[*] Restarting thread...\n");
				pthread_create(&threads[j], NULL, threadFunc, NULL);
			}
		}

		fd_set sockfds;
		FD_ZERO(&sockfds);
		FD_SET(web_sock, &sockfds);
		FD_SET(cmd_sock, &sockfds);
		int maxfd;
		if (web_sock > cmd_sock) {
			maxfd = web_sock;
		} else {
			maxfd = cmd_sock;
		}

		// Periodically unblock select to check terminated threads
		// The timeout has to be reinitialized after every call to select
		struct timeval timeout;
		timeout.tv_sec = 10;
		timeout.tv_usec = 0;

		if (select(maxfd + 1, &sockfds, NULL, NULL, &timeout) == -1) {
			perror("select");
			cleanup(threads, threadCount);
			close(web_sock);
			close(cmd_sock);
			return -2;
		}

		// Check which socket is ready to accept a connection
		// Handle command request first
		if (FD_ISSET(cmd_sock, &sockfds)) {
			client_sock = accept(cmd_sock, (struct sockaddr *) &client, &client_len);
			if (client_sock < 0 && errno != EAGAIN) {
				perror("accept");
				cleanup(threads, threadCount);
				close(web_sock);
				close(cmd_sock);
				return -2;
			} else if (client_sock >= 0) {
				// Get the IP of the connected client
				printf("[+] Client connected to COMMAND port from %s:%d\n", inet_ntoa(client.sin_addr), ntohs(client.sin_port));

				int res = handleCommand(client_sock, startTime);
				// Client socket no longer needed (one command per connection)
				close(client_sock);
				if (res == CMD_SHUTDOWN) {
					printf("[!] SHUTTING DOWN SERVER\n");
					break;
				}
			} else {
				printf("[!] Client closed the connection\n");
			}
		}

		// Handle web request
		if (FD_ISSET(web_sock, &sockfds)) {
			client_sock = accept(web_sock, (struct sockaddr *) &client, &client_len);
			if (client_sock < 0 && errno != EAGAIN) {
				perror("accept");
				cleanup(threads, threadCount);
				close(web_sock);
				close(cmd_sock);
				return -2;
			} else if (client_sock >= 0) {
				// Get the IP of the connected client
				printf("[+] Client connected to WEB port from %s:%d\n", inet_ntoa(client.sin_addr), ntohs(client.sin_port));

				if (handleRequest(client_sock, dirname) < 0) {
					fprintf(stderr, "[-] Error while handling request\n");
					cleanup(threads, threadCount);
					close(web_sock);
					close(cmd_sock);
					return -2;
				}
			} else {
				printf("[!] Client closed the connection\n");
			}
		}
	}


	cleanup(threads, threadCount);
	close(web_sock);
	close(cmd_sock);
	return 0;
}


/* Thread pool function */
void *threadFunc(void *ptr) {
	char *filename;
	int client_sock;
	int stop = 0;

	while (1) {
		// Each thread waits for a request to be added so that it can serve it
		pthread_mutex_lock(&queue_mtx);
		while (isEmpty(&reqQueue)) {
			pthread_cond_wait(&cond_nonempty, &queue_mtx);
		}

		// Check if the thread needs to stop before serving a request
		// in case it was signaled from the main thread
		pthread_mutex_lock(&thread_stop_mtx);
		stop = threadStop;
		pthread_mutex_unlock(&thread_stop_mtx);
		if (stop) {
			pthread_mutex_unlock(&queue_mtx);
			printf("[*] Thread %ld exiting...\n", pthread_self());
			pthread_exit(NULL);
		}

		queueRemove(&reqQueue, &filename, &client_sock);

		// No need to broadcast, only 1 producer
		pthread_cond_signal(&cond_nonfull);
		pthread_mutex_unlock(&queue_mtx);

		// Serve the client
		serveClient(filename, client_sock);
		free(filename);
		close(client_sock);
	}
}


/* Return the page requested to the client */
void serveClient(char *filename, int client_sock) {
	printf("[+] Thread: %ld serving page %s\n", pthread_self(), filename);

	// File not found
	if (access(filename, F_OK) == -1) {
		char msg[] = "<html><body><h3>404 Not Found</h3></body></html>";
		char *headers = createResponseHeaders(CODE_NOT_FOUND, strlen(msg));
		if (headers == NULL) {
			return;
		}

		int resSize = strlen(msg) + strlen(headers) + 1;
		char *response = malloc(resSize * sizeof(char));
		if (response == NULL) {
			perror("malloc");
			return;
		}

		strcpy(response, headers);
		strcat(response, msg);
		write(client_sock, response, strlen(response));

		free(headers);
		free(response);
		return;
	}


	struct stat fileStat;
	if (stat(filename, &fileStat) != 0) {
		perror("stat");
		return;
	}

	// File not readable or directory
	if (access(filename, R_OK) == -1 || !S_ISREG(fileStat.st_mode) || invalidFile(filename)) {
		char msg[] = "<html><body><h3>403 Forbidden</h3></body></html>";
		char *headers = createResponseHeaders(CODE_FORBIDDEN, strlen(msg));
		if (headers == NULL) {
			return;
		}

		int resSize = strlen(msg) + strlen(headers) + 1;
		char *response = malloc(resSize * sizeof(char));
		if (response == NULL) {
			perror("malloc");
			return;
		}

		strcpy(response, headers);
		strcat(response, msg);
		write(client_sock, response, strlen(response));

		free(headers);
		free(response);
		return;
	}

	// Send the requested page
	FILE *fp = fopen(filename, "r");
	if (fp == NULL) {
		perror("fopen");
		return;
	}

	// Get file size
	fseek(fp, 0, SEEK_END);
	int fileSize = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	// Load file contents in memory
	char *contents = malloc(fileSize * sizeof(char));
	if (contents == NULL) {
		perror("malloc");
		return;
	}

	fread(contents, sizeof(char), fileSize, fp);
	fclose(fp);

	// Send headers
	char *headers = createResponseHeaders(CODE_OK, fileSize);
	if (headers == NULL) {
		free(contents);
		return;
	}
	write(client_sock, headers, strlen(headers));
	free(headers);

	// Send file contents
	write(client_sock, contents, fileSize);
	free(contents);

	// Update stats
	pthread_mutex_lock(&stats_mtx);
	pagesServed++;
	bytesServed += fileSize;
	pthread_mutex_unlock(&stats_mtx);
}


int invalidFile(char *filename) {
	if (strstr(filename, "..") != NULL) {
		return 1;
	}

	int i;
	for (i = 0; i < strlen(filename); i++) {
		if (!isalnum(filename[i]) && filename[i] != '.' && filename[i] != '/' && filename[i] != '_') {
			return 1;
		}
	}

	return 0;
}


/* Check if a web request is valid and place it in the request queue so that a thread
 * can serve it */
int handleRequest(int client_sock, char *root_dir) {
	// Receive the request
	char *buf = malloc(BUF_SIZE * sizeof(char));
	if (buf == NULL) {
		perror("malloc");
		close(client_sock);
		return -1;
	}
	int bufSize = BUF_SIZE;
	int bufOffset = 0;
	int bytesRecv;

	while ((bytesRecv = read(client_sock, buf + bufOffset, bufSize - bufOffset - 1)) > 0) {
		// Resize buffer if needed
		while (bufOffset + bytesRecv >= bufSize - 1) {
			bufSize *= 2;
			buf = realloc(buf, bufSize);
			if (buf == NULL) {
				perror("realloc");
				close(client_sock);
				return -1;
			}
		}
		bufOffset += bytesRecv;

		buf[bufOffset] = '\0';
		// Check if we read "\r\n\r\n"
		if (strstr(buf, "\r\n\r\n") != NULL) {
			break;
		}
	}

	if (bytesRecv < 0 && errno != EAGAIN) {
		perror("read");
		close(client_sock);
		free(buf);
		return -1;
	}


	// Get the file requested
	char *req_file;
	if ((req_file = parseRequest(buf)) == NULL) {
		printf("[*] Received invalid request\n");

		// Send 400 Bad Request response
		char msg[] = "<html><body><h3>400 Bad Request</h3></body></html>";
		char *headers = createResponseHeaders(CODE_BAD, strlen(msg));
		if (headers != NULL) {
			int resSize = strlen(headers) + strlen(msg) + 1;
			char *response = malloc(resSize * sizeof(char));
			if (response != NULL) {
				strcpy(response, headers);
				strcat(response, msg);
				write(client_sock, response, resSize - 1);
				free(response);
			}
			free(headers);
		}

		close(client_sock);
		free(buf);
		return 1;
	}

	printf("[*] Received GET request for %s\n", req_file);

	// Create full path of requested file
	int size = strlen(root_dir) + strlen(req_file) + 1;
	char *filename = malloc(size * sizeof(char));
	strcpy(filename, root_dir);
	strcat(filename, req_file);
	free(req_file);
	free(buf);


	// Place the request in the request queue for a thread to serve it
	pthread_mutex_lock(&queue_mtx);
	// Wait for an empty position to be created in the request queue
	while (isFull(&reqQueue)) {
		pthread_cond_wait(&cond_nonfull, &queue_mtx);
	}
	queueInsert(&reqQueue, filename, client_sock);
	// Signal the threads so that they can serve the new request
	pthread_cond_signal(&cond_nonempty);
	pthread_mutex_unlock(&queue_mtx);

	free(filename);
	return 0;
}


/* Run a command sent through the command port */
int handleCommand(int client_sock, long long startTime) {
	char buf[32] = {0};
	read(client_sock, buf, 32);
	if (strncmp(buf, "STATS", 5) == 0) {
		printf("[*] Received STATS command\n");

		char msg[BUF_SIZE];
		// Get the current time in milliseconds
		struct timeval tv;
		gettimeofday(&tv, NULL);
		long long currTime = tv.tv_sec * 1000 + tv.tv_usec / 1000;
		// Get elaped time
		long long diff = currTime - startTime;
		long long secondsDiff = diff / 1000;

		int hours = secondsDiff / 3600;
		int minutes = (secondsDiff % 3600) / 60;
		int seconds = secondsDiff % 60;
		int milliseconds = diff % 1000;


		pthread_mutex_lock(&stats_mtx);
		int pages = pagesServed;
		int bytes = bytesServed;
		pthread_mutex_unlock(&stats_mtx);

		sprintf(msg, "Server up for %02i:%02i:%02i.%03i, served %d pages, %d bytes\n", hours, minutes, seconds, milliseconds, pages, bytes);
		write(client_sock, msg, strlen(msg));
		return CMD_OK;
	} else if (strncmp(buf, "SHUTDOWN", 8) == 0) {
		printf("[*] Received SHUTDOWN command\n");
		char msg[] = "\n*** SERVER SHUTTING DOWN ***\n";
		write(client_sock, msg, strlen(msg));
		return CMD_SHUTDOWN;
	} else {
		printf("[*] Received invalid command\n");
		char msg[] = "INVALID COMMAND\n";
		write(client_sock, msg, strlen(msg));
		return CMD_INVALID;
	}
}


/* Stop threads and free memory */
void cleanup(pthread_t *threads, int threadCount) {
	// Notify the threads to stop
	pthread_mutex_lock(&thread_stop_mtx);
	threadStop = 1;
	pthread_mutex_unlock(&thread_stop_mtx);

	// Unblock all threads so that they can read the exit flag
	// The threads will stop after the current requests have been served
	pthread_mutex_lock(&queue_mtx);
	while (isFull(&reqQueue)) {
		pthread_cond_wait(&cond_nonfull, &queue_mtx);
	}
	queueInsert(&reqQueue, "DUMMY", 0); // Dummy insert
	pthread_cond_broadcast(&cond_nonempty);
	pthread_mutex_unlock(&queue_mtx);

	// Wait for threads to finish ongoing requests and terminate
	int i;
	for (i = 0; i < threadCount; i++) {
		pthread_join(threads[i], NULL);
	}
	free(threads);

	// Free mutexes and condition variables
	pthread_mutex_destroy(&thread_stop_mtx);
	pthread_mutex_destroy(&stats_mtx);
	pthread_mutex_destroy(&queue_mtx);
	pthread_cond_destroy(&cond_nonempty);
	pthread_cond_destroy(&cond_nonfull);

	// Destroy the request queue
	// (No mutex needed since all threads have stopped)
	queueDestroy(&reqQueue);
}


void usage(char *name) {
	printf("Usage: %s -p <serving port> -c <command port> -t <num of threads> -d <root dir>\n", name);
}
