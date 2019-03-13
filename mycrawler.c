#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h> // isalnum
#include <unistd.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h> // waitpid
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/time.h> // gettimeofday
#include <limits.h> // PATH_MAX
#include <errno.h>
#include "url_queue.h"
#include "requests.h"
#include "hash_table.h"
#include "util.h"

#define DIR_PERMS 0700

#define BUF_SIZE 256

#define CMD_OK       0
#define CMD_SHUTDOWN 1
#define CMD_SEARCH   2
#define CMD_INVALID -1
#define CMD_ERROR   -2

#define MAX_KEYWORDS 10
#define DOCFILE "docfile.txt"
#define JE_DIR "./JE/"
#define READ  0
#define WRITE 1

static void *threadFunc(void *);
static void sendRequest(char *, char *);
static int createConnection(char *);
static char *readResponse(int);
static void parseContent(char *, char *);
static void saveFile(char *, char *, char *);
static int handleCommand(int, long long, char ***, int *);
static int hasData(int);
static int validUrl(char *, char *, int);
static void cleanup(pthread_t *, int, char *);
static void child_handler(int);
static void usage(char *);


// Used to stop threads when shutting down crawler
static int threadStop = 0;
static pthread_mutex_t thread_stop_mtx = PTHREAD_MUTEX_INITIALIZER;

// Used to check if crawling has finished
static int threadsInProgress = 0;
static pthread_mutex_t thread_progress_mtx = PTHREAD_MUTEX_INITIALIZER;

// Variables used for STATS command
static int pagesDownloaded = 0;
static int bytesDownloaded = 0;
static pthread_mutex_t stats_mtx = PTHREAD_MUTEX_INITIALIZER;

// Hash table of URLs to check if a URL has already been checked
static HashTable table;
static pthread_mutex_t table_mtx = PTHREAD_MUTEX_INITIALIZER;

// Queue of URLs to be requested by the threads
static URLQueue urlQueue;
static pthread_mutex_t queue_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_nonempty = PTHREAD_COND_INITIALIZER;

// Mutex used to update the docfile used by the Job Executor
static pthread_mutex_t docfile_mtx = PTHREAD_MUTEX_INITIALIZER;

// Job Executor PID
static pid_t JE_pid = -1;


int main(int argc, char *argv[]) {
	if (argc != 12) {
		usage(argv[0]);
		return -1;
	}

	// Ignore SIGPIPEs. We will handle the errors
	signal(SIGPIPE, SIG_IGN);

	char *host;
	int sport;
	int cport;
	int threadCount;
	char *startUrl = argv[argc-1];
	char *dirname;
	struct stat dirStat;

	// Parse arguments
	int got_host = 0;
	int got_sport = 0;
	int got_cport = 0;
	int got_threads = 0;
	int got_dir = 0;
	int i;
	for (i = 1; i < argc - 1; i += 2) {
		if (strcmp(argv[i], "-h") == 0 && !got_host) {
			got_host = 1;
			host = argv[i+1];

			int j;
			for (j = 0; j < strlen(host); j++) {
				if (!isalnum(host[i]) && host[i] != '.' && host[i] != '/' && host[i] != '-') {
					fprintf(stderr, "[-] Invalid host\n");
					return -1;
				}
			}
		} else if (strcmp(argv[i], "-p") == 0 && !got_sport) {
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

	if (!validUrl(startUrl, host, sport)) {
		fprintf(stderr, "[-] Invalid starting URL\n");
		return -1;
	}


	// Get the start time in milliseconds
	struct timeval tv;
	gettimeofday(&tv, NULL);
	long long startTime = tv.tv_sec * 1000 + tv.tv_usec / 1000;


	queueInit(&urlQueue);
	HT_initialize(&table);

	// Insert the starting URL in the URL Queue and the URL Hash Table
	queueInsert(&urlQueue, startUrl);
	HT_insert(&table, startUrl);

	// Check if the docfile for the JE already exists and remove it
	if (access(DOCFILE, F_OK) != -1) {
		if (remove(DOCFILE) == -1) {
			perror("remove");
			HT_destroy(&table);
			queueDestroy(&urlQueue);
			return -2;
		}
	}

	char *save_dir = malloc((strlen(dirname) + 1) * sizeof(char));
	strcpy(save_dir, dirname);
	// Remove save_dir and its contents if it already exists
	if (removeDirectory(save_dir) != 0) {
		fprintf(stderr, "[-] Failed to remove previous save_dir %s\n", save_dir);
		HT_destroy(&table);
		queueDestroy(&urlQueue);
		free(save_dir);
		return -2;
	}
	// Create new empty save_dir
	if (mkdir(save_dir, DIR_PERMS) != 0) {
		perror("mkdir");
		HT_destroy(&table);
		queueDestroy(&urlQueue);
		free(save_dir);
		return -2;
	}

	// Create the thread pool
	pthread_t *threads = malloc(threadCount * sizeof(pthread_t));
	for (i = 0; i < threadCount; i++) {
		pthread_create(&threads[i], NULL, threadFunc, save_dir);
	}



	// COMMAND SOCKET
	int reuse = 1;
	int cmd_sock;
	if ((cmd_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("socket: cmd");
		cleanup(threads, threadCount, save_dir);
		return -2;
	}
	// Avoid TIME_WAIT state
	if (setsockopt(cmd_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
		perror("setsockopt");
		cleanup(threads, threadCount, save_dir);
		close(cmd_sock);
		return -2;
	}

	struct sockaddr_in cmd_server;
	cmd_server.sin_family = AF_INET;
	cmd_server.sin_addr.s_addr = htonl(INADDR_ANY);
	cmd_server.sin_port = htons(cport);
	if (bind(cmd_sock, (struct sockaddr *) &cmd_server, sizeof(cmd_server)) < 0) {
		perror("bind: command");
		cleanup(threads, threadCount, save_dir);
		close(cmd_sock);
		return -2;
	}

	// Set the socket as non-blocking to handle client disconnection after select succeeds
	int opts = fcntl(cmd_sock, F_GETFL);
	if (opts < 0) {
		perror("fcntl: F_GETFL");
		cleanup(threads, threadCount, save_dir);
		close(cmd_sock);
		return -2;
	}
	if (fcntl(cmd_sock, F_SETFL, opts | O_NONBLOCK) < 0) {
		perror("fcntl: F_SETFL");
		cleanup(threads, threadCount, save_dir);
		close(cmd_sock);
		return -2;
	}

	if (listen(cmd_sock, 5) < 0) {
		perror("listen: command");
		cleanup(threads, threadCount, save_dir);
		close(cmd_sock);
		return -2;
	}
	printf("[+] Listening for commands on port %d\n\n", cport);



	int client_sock;
	struct sockaddr_in client;
	socklen_t client_len = sizeof(client);

	int checkThreads = 1;
	int startedJE = 0;
	int resultfd[2]; // Pipe used to receive JE's results
	int cmdfd[2]; // Pipe used to send commands to the JE
	while (1) {
		// Check if a thread was terminated and we need to create a new one
		// if the crawling hasn't finished
		if (checkThreads) {
			int finished = 0;
			pthread_mutex_lock(&thread_stop_mtx);
			finished = threadStop;
			pthread_mutex_unlock(&thread_stop_mtx);

			if (finished) {
				checkThreads = 0;
				printf("\n[...] Starting the Job Executor\n");

				//////// Start the Job Executor //////////////
				// Create the full docfile path
				char docfilePath[PATH_MAX];
				if (getcwd(docfilePath, PATH_MAX) == NULL) {
					perror("getcwd");
					cleanup(threads, threadCount, save_dir);
					close(cmd_sock);
					return -2;
				}
				strcat(docfilePath, "/");
				strcat(docfilePath, DOCFILE);

				// Set up SIGCHLD handler to check if the JE terminates unexpectedly
				struct sigaction act = {0};
				act.sa_handler = child_handler;
				sigemptyset(&(act.sa_mask));
				sigaddset(&(act.sa_mask), SIGCHLD);
				sigaction(SIGCHLD, &act, NULL);

				// Create the pipes used for communication between the Crawler and the JE
				if (pipe(resultfd) == -1) {
					perror("pipe: results");
					close(cmd_sock);
					cleanup(threads, threadCount, save_dir);
					return -2;
				}

				if (pipe(cmdfd) == -1) {
					perror("pipe: commands");
					close(cmd_sock);
					close(resultfd[READ]);
					close(resultfd[WRITE]);
					cleanup(threads, threadCount, save_dir);
					return -2;
				}

				// Create the Job Executor process with fork & exec
				pid_t pid = fork();
				if (pid == 0) { // JOB EXECUTOR
					close(cmd_sock);

					// Redirect JE's reading pipe to its stdin
					dup2(cmdfd[READ], STDIN_FILENO);
					close(cmdfd[READ]);
					close(cmdfd[WRITE]);
					// Redirect JE's stdout to its writing pipe
					dup2(resultfd[WRITE], STDOUT_FILENO);
					close(resultfd[READ]);
					close(resultfd[WRITE]);

					// Move to the Job Executor's directory
					if (chdir(JE_DIR) < 0) {
						perror("chdir");
						exit(-1);
					}

					if (execl("./jobExecutor", "./jobExecutor", "-d", docfilePath, "-w", "4", NULL) == -1) {
						perror("execl");
						exit(-1);
					}
				} else if (pid < 0) {
					perror("fork");
					close(cmd_sock);
					cleanup(threads, threadCount, save_dir);
					return -2;
				}


				// Close the unused ends of the pipes
				close(cmdfd[READ]);
				close(resultfd[WRITE]);

				// Read JE's startup messages
				char buf[BUF_SIZE];
				int bytesRead;
				do {
					bytesRead = read(resultfd[READ], buf, BUF_SIZE-1);
					buf[bytesRead] = '\0';
					printf("\nJOB EXECUTOR OUTPUT:\n%s\n", buf);
				} while (hasData(resultfd[READ]));

				JE_pid = pid;
				startedJE = 1;
				printf("[+] Job Executor started\n");
			} else {
				int j;
				for (j = 0; j < threadCount; j++) {
					// On success (0), the thread has been terminated
					if (pthread_tryjoin_np(threads[j], NULL) == 0) {
						printf("[-] A thread has been terminated\n");
						printf("[*] Restarting thread...\n");
						pthread_create(&threads[j], NULL, threadFunc, NULL);
					}
				}
			}
		}

		fd_set sockfds;
		FD_ZERO(&sockfds);
		FD_SET(cmd_sock, &sockfds);

		// Periodically unblock select to check terminated threads
		// The timeout has to be reinitialized after every call to select
		struct timeval timeout;
		timeout.tv_sec = 5;
		timeout.tv_usec = 0;

		if (select(cmd_sock + 1, &sockfds, NULL, NULL, &timeout) == -1) {
			perror("select");
			cleanup(threads, threadCount, save_dir);
			close(cmd_sock);
			close(cmdfd[WRITE]);
			close(resultfd[READ]);
			return -2;
		}

		// Handle command request
		if (FD_ISSET(cmd_sock, &sockfds)) {
			client_sock = accept(cmd_sock, (struct sockaddr *) &client, &client_len);
			if (client_sock < 0 && errno != EAGAIN) {
				perror("accept");
				cleanup(threads, threadCount, save_dir);
				close(cmd_sock);
				close(cmdfd[WRITE]);
				close(resultfd[READ]);
				return -2;
			} else if (client_sock >= 0) {
				// Get the IP of the connected client
				printf("[+] Client connected to COMMAND port from %s:%d\n", inet_ntoa(client.sin_addr), ntohs(client.sin_port));

				char **keywords = NULL;
				int numOfKeywords = 0;
				int res = handleCommand(client_sock, startTime, &keywords, &numOfKeywords);
				if (res == CMD_SHUTDOWN) {
					printf("[!] SHUTTING DOWN CRAWLER\n");
					close(client_sock);
					break;
				} else if (res == CMD_ERROR) {
					fprintf(stderr, "[-] Error while reading command\n");
					cleanup(threads, threadCount, save_dir);
					close(cmd_sock);
					close(client_sock);
				} else if (res == CMD_SEARCH) {
					// Check if crawling has finished
					int ready = 0;
					pthread_mutex_lock(&thread_stop_mtx);
					if (threadStop == 1) {
						ready = 1;
					}
					pthread_mutex_unlock(&thread_stop_mtx);

					// Crawling hasn't finished yet
					if (!ready) {
						char msg[] = "\nCRAWLING IN PROGRESS\n";
						write(client_sock, msg, strlen(msg));
						close(client_sock);
						int i;
						for (i = 0; i < numOfKeywords; i++) {
							free(keywords[i]);
						}
						free(keywords);
						continue;
					} else if (!startedJE) {
						char msg[] = "\nJOB EXECUTOR NOT READY YET\n";
						write(client_sock, msg, strlen(msg));
						close(client_sock);
						int i;
						for (i = 0; i < numOfKeywords; i++) {
							free(keywords[i]);
						}
						free(keywords);
						continue;
					}


					// Create the JE command string (/search <keyword1> <keyword2> ...)
					// with default timeout of 5 seconds
					int cmdSize = strlen("/search -d 5\n") + 1; // Include null-byte
					int i;
					for (i = 0; i < numOfKeywords; i++) {
						cmdSize += strlen(keywords[i]) + 1; // Include space after keyword
					}

					char *cmd = malloc(cmdSize * sizeof(char));
					strcpy(cmd, "/search ");
					for (i = 0; i < numOfKeywords; i++) {
						strcat(cmd, keywords[i]);
						strcat(cmd, " ");
					}
					strcat(cmd, "-d 5\n");

					// Send the command to the JE
					int remaining = cmdSize - 1;
					int curr = 0;
					while (remaining > 0) {
						int written = write(cmdfd[WRITE], cmd + curr, remaining);
						remaining -= written;
						curr += written;
					}
					free(cmd);

					// Get the results from the Job Executor and send them to the socket
					char buf[BUF_SIZE];
					int bytesRead;
					do {
						bytesRead = read(resultfd[READ], buf, BUF_SIZE-1);
						buf[bytesRead] = '\0';
						write(client_sock, buf, bytesRead);
					} while (hasData(resultfd[READ]));


					close(client_sock);
					for (i = 0; i < numOfKeywords; i++) {
						free(keywords[i]);
					}
					free(keywords);
				} else {
					close(client_sock);
				}
			} else {
				printf("[!] Client closed the connection\n");
			}
		}
	}


	cleanup(threads, threadCount, save_dir);
	close(cmd_sock);
	close(cmdfd[WRITE]);
	close(resultfd[READ]);
	return 0;
}


/* Thread pool function */
void *threadFunc(void *ptr) {
	char *save_dir = (char *) ptr;
	int stop = 0;
	while (1) {
		pthread_mutex_lock(&queue_mtx);
		while (isEmpty(&urlQueue)) {
			// Wait for a thread to add a URL in the queue
			pthread_cond_wait(&cond_nonempty, &queue_mtx);
		}

		// Check if the thread needs to stop before requesting a new page
		// in case it was signaled from the main thread or the crawling has finished
		pthread_mutex_lock(&thread_stop_mtx);
		stop = threadStop;
		pthread_mutex_unlock(&thread_stop_mtx);
		if (stop) {
			pthread_mutex_unlock(&queue_mtx);
			printf("[*] Thread %ld exiting...\n", pthread_self());
			pthread_exit(NULL);
		}

		// Get a URL from the queue
		char *url = queueRemove(&urlQueue);
		printf("[+] Thread %ld getting URL: %s\n", pthread_self(), url);

		// Increase threads in progress counter
		pthread_mutex_lock(&thread_progress_mtx);
		threadsInProgress++;
		pthread_mutex_unlock(&thread_progress_mtx);

		pthread_mutex_unlock(&queue_mtx);


		sendRequest(url, save_dir);
		free(url);

		pthread_mutex_lock(&thread_progress_mtx);
		threadsInProgress--;
		pthread_mutex_unlock(&thread_progress_mtx);
	}
}


/* Send a GET request to a URL and parse its response */
void sendRequest(char *url, char *save_dir) {
	// Copy URL to avoid changes from strtok_r
	char *urlcopy = malloc((strlen(url) + 1) * sizeof(char));
	strcpy(urlcopy, url);

	char *start = url + 7; // Ignore http://

	// Extract file from the URL
	char *filePos = strchr(start, '/');
	char *file = malloc((strlen(filePos) + 1) * sizeof(char));
	strcpy(file, filePos);

	// Extract host and port from the URL
	char *saveptr;
	char *host = strtok_r(start, "/", &saveptr);

	// Create the request
	char *req = createRequestHeaders(host, file);
	if (req == NULL) {
		fprintf(stderr, "[-] Error while creating request\n");
		free(urlcopy);
		free(file);
		return;
	}

	int server_sock = createConnection(host);
	if (server_sock < 0) {
		fprintf(stderr, "[-] Error while connecting\n");
		free(req);
		free(urlcopy);
		free(file);
		return;
	}

	// Send the request
	write(server_sock, req, strlen(req));
	free(req);

	// Read the response
	char *content = readResponse(server_sock);
	if (content == NULL) { // Error or page not found / page not accessible
		// Check if there are any other threads in progress
		int inProgress = 0;
		pthread_mutex_lock(&thread_progress_mtx);
		inProgress = threadsInProgress;
		pthread_mutex_unlock(&thread_progress_mtx);

		// We are the last thread. Crawling ended
		if (inProgress == 1) {
			printf("\n[+] Crawling has finsished\n");
			// Set the thread stop flag and unblock all threads
			pthread_mutex_lock(&thread_stop_mtx);
			threadStop = 1;
			pthread_mutex_unlock(&thread_stop_mtx);

			pthread_mutex_lock(&queue_mtx);
			queueInsert(&urlQueue, "DUMMY"); // Dummy insert
			pthread_cond_broadcast(&cond_nonempty);
			pthread_mutex_unlock(&queue_mtx);
		}
		free(urlcopy);
		free(file);
		return;
	}

	// Save the file inside the save_dir
	saveFile(content, save_dir, file + 1); // Start filename after first '/'
	free(file);

	// Update stats
	int bytes = strlen(content);
	pthread_mutex_lock(&stats_mtx);
	pagesDownloaded++;
	bytesDownloaded += bytes;
	pthread_mutex_unlock(&stats_mtx);

	// Extract the links from the page we received
	parseContent(content, urlcopy);
	free(urlcopy);
	free(content);
}


/* Create a socket to the given host */
int createConnection(char *host) {
	char *saveptr;
	// Split hostname and port
	char *hostname = strtok_r(host, ":", &saveptr);
	char *portStr = strtok_r(NULL, "", &saveptr);
	int port = atoi(portStr);
	if (port <= 0 || port > 65535) {
		return -1;
	}

	int sock;
	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("socket");
		return -1;
	}

	struct sockaddr_in server;
	server.sin_family = AF_INET;
	server.sin_port = htons(port);

	int res;
	// Invalid IP address -> Convert hostname to IP address
	if ((res = inet_pton(AF_INET, hostname, &(server.sin_addr))) == 0) {
		struct addrinfo hints;
		memset(&hints, 0, sizeof(hints));
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_family = AF_INET;
		struct addrinfo *info = NULL;

		if (getaddrinfo(hostname, NULL, &hints, &info) != 0) {
			perror("getaddrinfo");
			close(sock);
			return -1;
		}

		struct sockaddr_in *sa = (struct sockaddr_in *) info->ai_addr;
		sa->sin_port = htons(port);
		if ((connect(sock, info->ai_addr, info->ai_addrlen)) < 0) {
			perror("connect");
			close(sock);
			freeaddrinfo(info);
			return -1;
		}
		freeaddrinfo(info);
	} else if (res > 0) {
		if ((connect(sock, (struct sockaddr *) &server, sizeof(server))) < 0) {
			perror("connect");
			close(sock);
			return -1;
		}
	} else {
		perror("inet_pton");
		close(sock);
		return -1;
	}

	return sock;
}


/* Read HTTP response from a GET request and return the content */
char *readResponse(int sock) {
	int bufSize = BUF_SIZE;
	char *buf = malloc(bufSize * sizeof(char));
	if (buf == NULL) {
		perror("malloc");
		close(sock);
		return NULL;
	}

	int bufOffset = 0;
	int bytesRecv;
	char *headerEnd = NULL;

	while ((bytesRecv = read(sock, buf + bufOffset, bufSize - bufOffset - 1)) > 0) {
		// Check if we got 404 or 403 response
		if (bufOffset == 0) {
			if (strncmp(buf, "HTTP/1.1 200 OK", 15) != 0) {
				fprintf(stderr, "[-] Page not found or not accessible\n");
				free(buf);
				close(sock);
				return NULL;
			}
		}

		// Resize buffer if needed
		while (bufOffset + bytesRecv >= bufSize - 1) {
			bufSize *= 2;
			buf = realloc(buf, bufSize);
			if (buf == NULL) {
				perror("realloc");
				close(sock);
				return NULL;
			}
		}
		bufOffset += bytesRecv;

		buf[bufOffset] = '\0';
		// Check if we reached the end of the HTTP headers
		if ((headerEnd = strstr(buf, "\r\n\r\n")) != NULL) {
			break;
		}
	}

	if (bytesRecv < 0 && errno != EAGAIN) {
		perror("read");
		close(sock);
		free(buf);
		return NULL;
	}


	// Find Content-Length header to check how much is left to read
	char *contentLengthHeader = strstr(buf, "Content-Length:");
	if (contentLengthHeader == NULL) {
		fprintf(stderr, "[-] Received invalid response\n");
		close(sock);
		free(buf);
		return NULL;
	}

	char *saveptr;
	// Get length field
	strtok_r(contentLengthHeader, ": ", &saveptr);
	char *lengthStr = strtok_r(NULL, "", &saveptr);
	int contentLength = atoi(lengthStr);
	if (contentLength <= 0) {
		fprintf(stderr, "[-] Received invalid response\n");
		close(sock);
		free(buf);
		return NULL;
	}

	char *content = malloc((contentLength + 1) * sizeof(char));
	// Check how much of the content we have read and how much is remaining
	headerEnd += 4; // Skip \r\n\r\n and go to the start of the content
	int contentRead = (int) (buf + bufOffset - headerEnd);

	// Copy the content to a new string
	if (contentRead > 0) {
		memcpy(content, headerEnd, contentRead);
	}
	free(buf);

	// Read the rest of the content
	int remainingContent = contentLength - contentRead;
	int offset = contentRead;
	while (remainingContent > 0) {
		bytesRecv = read(sock, content + offset, remainingContent);
		if (bytesRecv == -1) {
			perror("read");
			free(content);
			close(sock);
			return NULL;
		} else if (bytesRecv == 0) {
			break;
		}
		remainingContent -= bytesRecv;
		offset += bytesRecv;
	}
	// Null termination
	content[offset] = '\0';

	close(sock);
	return content;
}


/* Read response content and place the new links in the URL queue
 * if we haven't visited them already */
void parseContent(char *content, char *fullUrl) {
	// Create the base URL by adding a NULL byte at the start of the file path('/')
	char *path = strchr(fullUrl + 7, '/'); // Ignore http://
	*path = '\0';
	path++; // Start after first '/'
	char *baseUrl = fullUrl;
	// Get the directory from the path
	char *saveptr;
	char *dir = strtok_r(path, "/", &saveptr);

	int linkCount = 0;
	int linksSize = BUF_SIZE;
	char **links = malloc(linksSize * sizeof(char *));

	// Find the links in the text
	// <a href="link"> link_text </a>
	char *curr = content;
	char *link;
	while ((link = strstr(curr, "<a")) != NULL) {
		// Move the current position so that we don't search for the same
		// <a on the next loop
		curr++; // Don't find the same </a>
		curr = strstr(curr, "</a>");
		// No ending tag
		if (curr == NULL) {
			fprintf(stderr, "[-] Found link without ending </a> tag\n");
			int i;
			for (i = 0; i < linkCount; i++) {
				free(links[i]);
			}
			free(links);
			return;
		}

		// Find the href part of the link
		strtok_r(link, " ", &saveptr);
		char *href = strtok_r(NULL, " >", &saveptr);
		if (href == NULL || (strncmp(href, "href=\"", 6) != 0 && strncmp(href, "href='", 6) != 0)) {
			// No href attribute found, continue to the next link
			continue;
		}

		// Get the link inside the href attribute
		strtok_r(href, "'\"", &saveptr);
		char *file = strtok_r(NULL, "'\"", &saveptr);
		if (file == NULL) {
			continue;
		}

		// Create the full URL
		char *url;
		// Check if we have external or internal link
		if (file[0] == '/') { // External link
			url = malloc((strlen(baseUrl) + strlen(file) + 1) * sizeof(char));
			strcpy(url, baseUrl);
			strcat(url, file);
		} else { // Internal link
			url = malloc((strlen(baseUrl) + 1 + strlen(dir) + 1 + strlen(file) + 1) * sizeof(char));
			strcpy(url, baseUrl);
			strcat(url, "/");
			strcat(url, dir);
			strcat(url, "/");
			strcat(url, file);
		}

		// Add it to the URL array
		if (linkCount >= linksSize) {
			linksSize *= 2;
			links = realloc(links, linksSize);
			if (links == NULL) {
				perror("realloc");
				return;
			}
		}

		links[linkCount++] = url;
	}

	int newLinks = linkCount;

	// Add the links to the URL Queue
	int i;
	pthread_mutex_lock(&queue_mtx);
	for (i = 0; i < linkCount; i++) {
		// Check if the URL has already been visited
		pthread_mutex_lock(&table_mtx);
		if (HT_insert(&table, links[i]) == -1) {
			pthread_mutex_unlock(&table_mtx);
			newLinks--;
			continue;
		}
		pthread_mutex_unlock(&table_mtx);

		// Check if the URL is already in the URL Queue
		if (queueExists(&urlQueue, links[i])) {
			pthread_mutex_unlock(&queue_mtx);
			newLinks--;
			continue;
		}

		// New URL -> Add it in the URL Queue
		queueInsert(&urlQueue, links[i]);
	}
	// Notify the rest of the threads to read the new URLs
	// or to exit if the crawling has finished
	if (newLinks > 0) {
		pthread_cond_broadcast(&cond_nonempty);
	} else if (isEmpty(&urlQueue)) {
		// Check if there are any other threads in progress
		int inProgress = 0;
		pthread_mutex_lock(&thread_progress_mtx);
		inProgress = threadsInProgress;
		pthread_mutex_unlock(&thread_progress_mtx);

		// We are the last thread. Crawling ended
		if (inProgress == 1) {
			printf("\n[+] Crawling has finsished\n");
			// Set the thread stop flag and unblock all threads
			pthread_mutex_lock(&thread_stop_mtx);
			threadStop = 1;
			pthread_mutex_unlock(&thread_stop_mtx);

			queueInsert(&urlQueue, "DUMMY"); // Dummy insert
			pthread_cond_broadcast(&cond_nonempty);
		}
	}
	pthread_mutex_unlock(&queue_mtx);


	for (i = 0; i < linkCount; i++) {
		free(links[i]);
	}
	free(links);
}


/* Save a web page inside the save_dir and if the directory in which we found the page
 * is new, add it to the docfile used by the Job Executor when searching */
void saveFile(char *content, char *save_dir, char *filename) {
	// Get number of directories needed to save the page
	int count = 0;
	char *pos = filename;
	while ((pos = strchr(pos, '/')) != NULL) {
		pos++;
		count++;
	}

	// Copy filename to avoid changes from strtok_r
	char *path = malloc((strlen(filename) + 1) * sizeof(char));
	strcpy(path, filename);

	char *fullPath = malloc((strlen(save_dir) + 1 + strlen(filename) + 1) * sizeof(char));
	strcpy(fullPath, save_dir);
	strcat(fullPath, "/");
	int i;
	char *saveptr;
	// Create the directories needed if they don't already exist
	// and the entries in the docfile
	for (i = 0; i < count; i++) {
		char *dir;
		if (i == 0) {
			dir = strtok_r(path, "/", &saveptr);
		} else {
			dir = strtok_r(NULL, "/", &saveptr);
		}
		strcat(fullPath, dir);
		strcat(fullPath, "/");

		// Check if the directory already exists
		if (access(fullPath, F_OK) == -1) {
			// Create the directory
			if (mkdir(fullPath, DIR_PERMS) == -1) {
				perror("mkdir");
				free(path);
				free(fullPath);
				return;
			}

			// Append the directory to the docfile
			// (One thread can access it at a time
			pthread_mutex_lock(&docfile_mtx);
			FILE *fp = fopen(DOCFILE, "a");
			if (fp == NULL) {
				perror("fopen");
				free(path);
				free(fullPath);
				pthread_mutex_unlock(&docfile_mtx);
				return;
			}
			// Create absolute path
			char absPath[PATH_MAX];
			if (getcwd(absPath, PATH_MAX) == NULL) {
				perror("getcwd");
				free(path);
				free(fullPath);
				fclose(fp);
				pthread_mutex_unlock(&docfile_mtx);
				return;
			}
			strcat(absPath, "/");
			strcat(absPath, fullPath);
			fputs(absPath, fp);
			fputc('\n', fp);
			fclose(fp);
			pthread_mutex_unlock(&docfile_mtx);
		}
	}

	char *page;
	if (count == 0) {
		page = strtok_r(path, "", &saveptr);
	} else {
		page = strtok_r(NULL, "", &saveptr);
	}
	strcat(fullPath, page);
	free(path);

	// Write the contents to the file
	FILE *fp = fopen(fullPath, "w");
	if (fp == NULL) {
		perror("fopen");
		free(fullPath);
		return;
	}
	free(fullPath);

	fwrite(content, sizeof(char), strlen(content), fp);
	fclose(fp);
}


/* Run a command sent through the command port */
int handleCommand(int client_sock, long long startTime, char **retKeywords[], int *numOfKeywords) {
	int bufSize = BUF_SIZE;
	char *buf = malloc(bufSize * sizeof(char));
	if (buf == NULL) {
		perror("malloc");
		return CMD_ERROR;
	}

	int bufOffset = 0;
	int bytesRecv;

	// Keep reading bytes from the socket until we read a newline character
	while ((bytesRecv = read(client_sock, buf + bufOffset, bufSize - bufOffset - 1)) > 0) {
		// Resize buffer if needed
		while (bufOffset + bytesRecv >= bufSize - 1) {
			bufSize *= 2;
			buf = realloc(buf, bufSize);
			if (buf == NULL) {
				perror("realloc");
				return -1;
			}
		}
		bufOffset += bytesRecv;

		buf[bufOffset] = '\0';
		// Check if we read '\n'
		char *newlinePos = strchr(buf, '\n');
		if (newlinePos != NULL) {
			*newlinePos = '\0';
			break;
		}
	}

	if (bytesRecv < 0 && errno != EAGAIN) {
		perror("read");
		free(buf);
		return CMD_ERROR;
	}

	// Read the command and its arguments
	char *saveptr;
	char *cmd = strtok_r(buf, " ", &saveptr);
	if (cmd == NULL) {
		printf("[*] Received invalid command\n");
		char msg[] = "INVALID COMMAND\n";
		write(client_sock, msg, strlen(msg));
		return CMD_INVALID;
	}

	if (strncmp(cmd, "STATS", 5) == 0) {
		printf("[*] Received STATS command\n");

		char msg[BUF_SIZE];
		// Get current time in milliseconds
		struct timeval tv;
		gettimeofday(&tv, NULL);
		long long currTime = tv.tv_sec * 1000 + tv.tv_usec / 1000;
		// Get the elapsed time
		long long diff = currTime - startTime;
		long long secondsDiff = diff / 1000;

		int hours = secondsDiff / 3600;
		int minutes = (secondsDiff % 3600) / 60;
		int seconds = secondsDiff % 60;
		int milliseconds = diff % 1000;


		pthread_mutex_lock(&stats_mtx);
		int pages = pagesDownloaded;
		int bytes = bytesDownloaded;
		pthread_mutex_unlock(&stats_mtx);

		sprintf(msg, "Crawler up for %02i:%02i:%02i.%03i, downloaded %d pages, %d bytes\n", hours, minutes, seconds, milliseconds, pages, bytes);
		write(client_sock, msg, strlen(msg));
		free(buf);
		return CMD_OK;
	} else if (strncmp(cmd, "SHUTDOWN", 8) == 0) {
		printf("[*] Received SHUTDOWN command\n");
		char msg[] = "\n*** CRAWLER SHUTTING DOWN ***\n";
		write(client_sock, msg, strlen(msg));
		free(buf);
		return CMD_SHUTDOWN;
	} else if (strncmp(cmd, "SEARCH", 6) == 0) {
		printf("[*] Received SEARCH command\n");

		char **keywords;
		keywords = malloc(MAX_KEYWORDS * sizeof(char *));
		if (keywords == NULL) {
			perror("malloc");
			free(buf);
			return CMD_ERROR;
		}

		// Read the keywords
		char *arg = strtok_r(NULL, " ", &saveptr);
		if (arg == NULL) {
			char msg[] = "NO ARGUMENTS GIVEN\n";
			write(client_sock, msg, strlen(msg));
			free(buf);
			free(keywords);
			return CMD_INVALID;
		}
		keywords[0] = malloc((strlen(arg) + 1) * sizeof(char));
		if (keywords[0] == NULL) {
			perror("malloc");
			free(buf);
			free(keywords);
			return CMD_ERROR;
		}
		strcpy(keywords[0], arg);
		int currKeyword = 1;

		while (currKeyword < MAX_KEYWORDS && (arg = strtok_r(NULL, " ", &saveptr)) != NULL) {
			keywords[currKeyword] = malloc((strlen(arg) + 1) * sizeof(char));
			if (keywords[currKeyword] == NULL) {
				perror("malloc");
				free(buf);
				int i;
				for (i = 0; i < currKeyword; i++) {
					free(keywords[i]);
				}
				free(keywords);
				return CMD_ERROR;
			}
			strcpy(keywords[currKeyword], arg);
			currKeyword++;
		}
		*numOfKeywords = currKeyword;
		*retKeywords = keywords;

		free(buf);
		return CMD_SEARCH;
	} else {
		printf("[*] Received invalid command\n");
		char msg[] = "INVALID COMMAND\n";
		write(client_sock, msg, strlen(msg));
		free(buf);
		return CMD_INVALID;
	}
}


/* Check if the given file descriptor has data available to be read
 * without blocking using select */
int hasData(int fd) {
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(fd, &fds);

	struct timeval timeout;
	timeout.tv_sec = 5; // Same as JE search timeout
	timeout.tv_usec = 0;

	int res = select(fd + 1, &fds, NULL, NULL, &timeout);
	if (res == -1) {
		perror("select");
		if (JE_pid > 0) {
			kill(JE_pid, SIGTERM);
		}
		exit(-1);
	} else {
		// On timeout select returns 0
		return (res == 1);
	}
}


/* Check if a URL is valid.
 * Correct format: http://linux01.di.uoa.gr:8080/site1/page0_1234.html */
int validUrl(char *url, char *host, int portArg) {
	if (strncmp(url, "http://", 7) != 0) { // Check if URL starts with http://
		return 0;
	}
	if (strchr(url + 7, ':') == NULL) { // Check if port exists
		return 0;
	}

	// Copy string to avoid changes from strtok_r
	char *urlcopy = malloc((strlen(url) + 1) * sizeof(char));
	strcpy(urlcopy, url);

	char *saveptr;
	// Check hostname / IP part of the URL
	char *hostname = strtok_r(urlcopy + 7, ":", &saveptr);
	if (hostname == NULL) {
		free(urlcopy);
		return 0;
	}
	// Check if URL host matches the host argument
	if (strcmp(hostname, host) != 0) {
		free(urlcopy);
		return 0;
	}

	// Check if port matches the port argument
	char *port = strtok_r(NULL, "/", &saveptr);
	if (port == NULL || atoi(port) != portArg) {
		free(urlcopy);
		return 0;
	}

	// Check the file part of the URL
	char *file = strtok_r(NULL, "", &saveptr);
	if (file == NULL) {
		free(urlcopy);
		return 0;
	}

	int i;
	for (i = 0; i < strlen(file); i++) {
		if (!isalnum(file[i]) && file[i] != '-' && file[i] != '_' && file[i] != '.' && file[i] != '/') {
			free(urlcopy);
			return 0;
		}
	}

	free(urlcopy);
	return 1;
}


void cleanup(pthread_t *threads, int threadCount, char *save_dir) {
	// Notify the threads to stop
	pthread_mutex_lock(&thread_stop_mtx);
	threadStop = 1;
	pthread_mutex_unlock(&thread_stop_mtx);

	// Unblock all threads so that they can read the exit flag
	// The threads will stop after the current requests have finished
	pthread_mutex_lock(&queue_mtx);
	queueInsert(&urlQueue, "DUMMY"); // Dummy insert
	pthread_cond_broadcast(&cond_nonempty);
	pthread_mutex_unlock(&queue_mtx);

	// Wait for threads to exit
	int i;
	for (i = 0; i < threadCount; i++) {
		pthread_join(threads[i], NULL);
	}
	free(threads);
	free(save_dir);

	pthread_mutex_destroy(&thread_stop_mtx);
	pthread_mutex_destroy(&thread_progress_mtx);
	pthread_mutex_destroy(&stats_mtx);
	pthread_mutex_destroy(&table_mtx);
	pthread_mutex_destroy(&queue_mtx);
	pthread_cond_destroy(&cond_nonempty);

	queueDestroy(&urlQueue);
	HT_destroy(&table);

	// Disable SIGCHLD and kill the Job Executor
	signal(SIGCHLD, SIG_IGN);
	if (JE_pid > 0) {
		kill(JE_pid, SIGTERM);
	}
}


void child_handler(int sig) {
	if (sig == SIGCHLD) {
		// Check if we really received SIGCHLD from the JE
		int status = 0;
		pid_t pid = waitpid(-1, &status, WNOHANG);
		if (pid == 0) { // False alarm, no child quit
			return;
		} else if (pid == -1) {
			perror("waitpid");
			exit(-100);
		}

		char msg[] = "\n[!] JOB EXECUTOR TERMINATED\n";
		write(STDOUT_FILENO, msg, strlen(msg));
		exit(-10);
	}
}


void usage(char *name) {
	printf("Usage: %s -h <host or IP> -p <port> -c <command port> -t <num of threads> -d <save dir> <starting URL>\n", name);
}
