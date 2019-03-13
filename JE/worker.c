#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h> // signals
#include <unistd.h> // read, write, close
#include <fcntl.h> // open
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h> // time, asctime
#include "worker.h"
#include "trie.h"
#include "comm.h"
#include "textfile.h"
#include "util.h"

#define BUF_SIZE           256
#define LOG_PREFIX   "Worker_"
#define PARENT_CHECK_TIME   30 // seconds


static void signal_handler(int, siginfo_t *, void *);
static int recvCommand(int, int, Trie *, FileList *, KeywordList *, int, int, int, sigset_t *, FILE *);
static char *replaceColons(char *);
static void writeLog(char *, FILE *);
static int getFilesInDir(char *, Trie *, int *, int *, int *, FileList **);
static void addFileNode(FileList **, char *, char **, int);
static char **getFileContents(FileList *, char *);
static void freeFileList(FileList *);
static void addKeyword(KeywordList *, char *);
static void freeKeywordList(KeywordList *);

// Global variables (used by signal handler)
static Trie *g_trie = NULL;
// Sorted list of filenames and file lines
static FileList *g_fileList = NULL;
// List of different keywords searched
static KeywordList g_kwList;

static int g_readfd = -1;
static int g_writefd = -1;

static char *g_logname = NULL;
static FILE *g_logfp = NULL;

static volatile sig_atomic_t g_cmdReady = 0; // Command requested
static volatile sig_atomic_t g_timedout = 0; // Used for search deadline
static volatile sig_atomic_t g_parentPid = 0;


int startWorker(char *readFifo, char *writeFifo) {
	// Setup signal handler
	struct sigaction act = {0};
	sigemptyset(&(act.sa_mask));
	sigaddset(&(act.sa_mask), SIGHUP);
	sigaddset(&(act.sa_mask), SIGINT);
	sigaddset(&(act.sa_mask), SIGTERM);
	sigaddset(&(act.sa_mask), SIGUSR1);
	sigaddset(&(act.sa_mask), SIGUSR2);
	sigaddset(&(act.sa_mask), SIGALRM); // Periodically check if parent died
	sigaddset(&(act.sa_mask), SIGSEGV); // Show SEGFAULT in child
	act.sa_sigaction = signal_handler;
	act.sa_flags |= SA_SIGINFO; // Get pid of signal sender
	sigaction(SIGHUP, &act, NULL);
	sigaction(SIGINT, &act, NULL);
	sigaction(SIGTERM, &act, NULL);
	sigaction(SIGUSR1, &act, NULL);
	sigaction(SIGUSR2, &act, NULL);
	sigaction(SIGALRM, &act, NULL);
	sigaction(SIGSEGV, &act, NULL);

	// Unblock signals in case worker is restarted from job executor
	// (signal handler blocks signals)
	sigset_t unblock;
	sigfillset(&unblock);
	sigprocmask(SIG_UNBLOCK, &unblock, NULL);

	// Ignore SIGPIPE
	struct sigaction ign = {0};
	sigemptyset(&(ign.sa_mask));
	sigaddset(&(ign.sa_mask), SIGPIPE);
	ign.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &ign, NULL);

	// Create set of signals to be blocked while writing to log
	sigset_t blockset;
	sigemptyset(&blockset);
	sigaddset(&blockset, SIGHUP);
	sigaddset(&blockset, SIGINT);
	sigaddset(&blockset, SIGTERM);

	alarm(PARENT_CHECK_TIME);

	g_parentPid = getppid();
	// Initialize the trie used to store words
	initialize(&g_trie);
	// Initialize keyword list
	g_kwList.head = NULL;
	g_kwList.numOfKeywords = 0;

	// Create log
	pid_t pid = getpid();
	int pidLength = digits(pid);
	g_logname = malloc((strlen(LOG_PATH) + strlen(LOG_PREFIX) + pidLength + 1) * sizeof(char));
	sprintf(g_logname, "%s%s%d", LOG_PATH, LOG_PREFIX, pid);
	g_logfp = fopen(g_logname, "w");
	if (g_logfp == NULL) {
		destroy(g_trie);
		free(g_logname);
		return -1;
	}



	// Open FIFO for reading to start reading directory list
	// (Will block until job executor is ready to open for writing)
	if ((g_readfd = open(readFifo, O_RDONLY)) < 0) {
		perror("Worker open : read");
		destroy(g_trie);
		fclose(g_logfp);
		unlink(g_logname);
		free(g_logname);
		return -1;
	}
	// Open FIFO for writing
	// (Will block until job executor is ready to open for reading)
	if ((g_writefd = open(writeFifo, O_WRONLY)) < 0) {
		perror("Worker open : write");
		close(g_readfd);
		destroy(g_trie);
		fclose(g_logfp);
		unlink(g_logname);
		free(g_logname);
		return -1;
	}
	free(readFifo);
	free(writeFifo);

	// Receive directory list
	int numberOfDirs = -1;
	char **directoryList = fifoRecv(g_readfd, &numberOfDirs);
	if (numberOfDirs <= 0 || directoryList == NULL) {
		fprintf(stderr, "[-] Error while reading directories from FIFO\n");
		close(g_readfd);
		close(g_writefd);
		destroy(g_trie);
		fclose(g_logfp);
		unlink(g_logname);
		free(g_logname);
		return -2;
	}


	// Read files from directory list and keep statistics
	int totalBytes = 0;
	int totalWords = 0;
	int totalLines = 0;
	int i;
	for (i = 0; i < numberOfDirs; i++) {
		int tempBytes = 0;
		int tempWords = 0;
		int tempLines = 0;

		if (getFilesInDir(directoryList[i], g_trie, &tempBytes, &tempWords, &tempLines, &g_fileList) < 0) {
			fprintf(stderr, "[-] Error while reading directory: %s\n", directoryList[i]);
			continue;
		}

		totalBytes += tempBytes;
		totalWords += tempWords;
		totalLines += tempLines;
	}

	for (i = 0; i < numberOfDirs; i++) {
		free(directoryList[i]);
	}
	free(directoryList);


	while (1) {
		// Wait for command signal (SIGUSR1)
		while (!g_cmdReady) {
			pause();
		}
		if (recvCommand(g_readfd, g_writefd, g_trie, g_fileList, &g_kwList, totalBytes, totalWords, totalLines, &blockset, g_logfp) < 0) {
			raise(SIGTERM);
		}
		g_cmdReady = 0;
	}

	return 0;
}


/* Receive command from the reading FIFO and return the result through the writing FIFO */
int recvCommand(int readfd, int writefd, Trie *trie, FileList *fileList, KeywordList *kwList, int totalBytes, int totalWords, int totalLines, sigset_t *blockset, FILE *logfp) {
	int numberOfLines;
	char **cmd = fifoRecv(readfd, &numberOfLines);
	if (cmd == NULL) {
		return -1;
	}

	char *buf = NULL;
	int bufSize;
	char *logStr = NULL;

	if (strcmp(cmd[0], "CMD:SEARCH") == 0) {
		// Block deadline signal until worker is ready to send results
		sigset_t blockdl;
		sigemptyset(&blockdl);
		sigaddset(&blockdl, SIGUSR2);
		sigprocmask(SIG_SETMASK, &blockdl, NULL);

		PostingsNode **totalNodes = malloc((numberOfLines - 1) * sizeof(PostingsNode *));
		int i;
		for (i = 1; i < numberOfLines; i++) {
			PostingsList *currPost = findPostingsList(trie, cmd[i]);
			if (currPost != NULL) {
				// Add the search term to the keywords found
				addKeyword(kwList, cmd[i]);
				totalNodes[i-1] = currPost->head;
			} else {
				totalNodes[i-1] = NULL;
			}
		}

		// Update the log with the files found for each term
		sigprocmask(SIG_SETMASK, blockset, NULL);
		// Find total line size
		for (i = 1; i < numberOfLines; i++) {
			// Filter out colons from keyword
			char *keyword = replaceColons(cmd[i]);
			int logSize = strlen("search : ") + strlen(keyword) + 3;

			PostingsNode *curr = totalNodes[i-1];
			if (curr == NULL) { // Word not found
				logSize++; // Null byte
				logStr = malloc(logSize * sizeof(char));
				sprintf(logStr, "search : %s : ", keyword);
				writeLog(logStr, logfp);
				free(logStr);
				free(keyword);
				continue;
			}

			// Calculate log string size
			while (curr != NULL) {
				logSize += strlen(curr->path);
				if (curr->next != NULL) {
					logSize++; // Add space if not last file
				}
				curr = curr->next;
			}
			logSize++; // Null byte
			// Create log string
			logStr = malloc(logSize * sizeof(char));
			sprintf(logStr, "search : %s : ", keyword);
			curr = totalNodes[i-1];
			while (curr != NULL) {
				strcat(logStr, curr->path);
				if (curr->next != NULL) {
					strcat(logStr, " ");
				}
				curr = curr->next;
			}
			writeLog(logStr, logfp);
			free(logStr);
			free(keyword);
		}
		sigprocmask(SIG_UNBLOCK, blockset, NULL);

		// Merge the postings lists and remove the duplicates
		PostingsList *newList = mergePostingsLists(totalNodes, numberOfLines-1);

		if (newList == NULL) { // No keywords found
			fifoSend(writefd, NULL, 0, 1);
		} else {
			// Send the results to the JE in the form:
			// <filename> <SEARCH_SEP> <line> <SEARCH_SEP> <line contents> <NULL BYTE>

			// Calculate the size of the buffer we are going to send
			PostingsNode *curr = newList->head;
			bufSize = 0;
			while (curr != NULL) {
				// Get file contents
				char **lines = getFileContents(fileList, curr->path);
				// Add size for every line
				LineList *lineList = (curr->fInfo).lineList;
				while (lineList != NULL) {
					bufSize += strlen(curr->path) + 1 + digits(lineList->line + 1) + 1 + strlen(lines[lineList->line]) + 1;
					lineList = lineList->next;
				}
				curr = curr->next;
			}

			buf = malloc(bufSize * sizeof(char));

			// Add files and lines to the buffer
			curr = newList->head;
			int offset = 0;
			while (curr != NULL) {
				// Get file contents
				char **lines = getFileContents(fileList, curr->path);
				LineList *lineList = (curr->fInfo).lineList;
				while (lineList != NULL) {
					sprintf(buf + offset, "%s%c%d%c%s", curr->path, (char) SEARCH_SEP, lineList->line + 1, (char) SEARCH_SEP, lines[lineList->line]);
					offset += strlen(curr->path) + 1 + digits(lineList->line + 1) + 1 + strlen(lines[lineList->line]) + 1;
					lineList = lineList->next;
				}
				curr = curr->next;
			}


			// Receive pending deadline signal
			sigprocmask(SIG_UNBLOCK, &blockdl, NULL);

			// If we didn't reach the deadline yet send the results to the JE
			if (!g_timedout) {
				if (fifoSend(writefd, buf, bufSize, 1) == -1) {
					free(buf);
					free(totalNodes);
					freePostings(newList);
					g_timedout = 0;
					return -1;
				}
			// Else, send nothing
			} else {
				if (fifoSend(writefd, NULL, 0, 1) == -1) {
					free(buf);
					free(totalNodes);
					freePostings(newList);
					g_timedout = 0;
					return -1;
				}
			}
		}
		g_timedout = 0;

		free(totalNodes);
		freePostings(newList);
	} else if (strcmp(cmd[0], "CMD:MAXCOUNT") == 0) {
		int count;
		char *filename = getMaxMinCount(trie, cmd[1], &count, MAXCOUNT);

		if (count == 0) { // Word not found
			filename = "NOT_FOUND";
		}

		// Update log
		sigprocmask(SIG_SETMASK, blockset, NULL);

		// Filter out colons for log
		char *keyword = replaceColons(cmd[1]);

		logStr = malloc(strlen("maxcount") + 3 + strlen(keyword) + 3 + strlen(filename) + 1);
		if (count > 0) {
			sprintf(logStr, "maxcount : %s : %s", keyword, filename);
		} else {
			sprintf(logStr, "maxcount : %s : ", keyword);
		}

		writeLog(logStr, logfp);
		sigprocmask(SIG_UNBLOCK, blockset, NULL);
		free(logStr);
		free(keyword);

		bufSize = strlen(filename) + 1 + digits(count) + 1;
		buf = malloc(bufSize * sizeof(char));
		sprintf(buf, "%s %d", filename, count);

		if (fifoSend(writefd, buf, bufSize, 1) == -1) {
			free(buf);
			return -1;
		}
	} else if (strcmp(cmd[0], "CMD:MINCOUNT") == 0) {
		int count;
		char *filename = getMaxMinCount(trie, cmd[1], &count, MINCOUNT);

		if (count == 0) { // Word not found
			filename = "NOT_FOUND";
		}

		// Update log
		sigprocmask(SIG_SETMASK, blockset, NULL);

		// Filter out colons for log
		char *keyword = replaceColons(cmd[1]);

		logStr = malloc(strlen("mincount") + 3 + strlen(keyword) + 3 + strlen(filename) + 1);
		if (count > 0) {
			sprintf(logStr, "mincount : %s : %s", keyword, filename);
		} else {
			sprintf(logStr, "mincount : %s : ", keyword);
		}

		writeLog(logStr, logfp);
		sigprocmask(SIG_UNBLOCK, blockset, NULL);
		free(logStr);
		free(keyword);

		bufSize = strlen(filename) + 1 + digits(count) + 1;
		buf = malloc(bufSize * sizeof(char));
		sprintf(buf, "%s %d", filename, count);

		if (fifoSend(writefd, buf, bufSize, 1) == -1) {
			free(buf);
			return -1;
		}
	} else if (strcmp(cmd[0], "CMD:WC") == 0) {
		bufSize = digits(totalBytes) + 1 + digits(totalWords) + 1 + digits(totalLines) + 1;

		// Update log
		sigprocmask(SIG_SETMASK, blockset, NULL);
		logStr = malloc(5 + digits(totalBytes) + 3 + digits(totalWords) + 3 + digits(totalLines) + 1);
		sprintf(logStr, "wc : %d : %d : %d", totalBytes, totalWords, totalLines);

		writeLog(logStr, logfp);
		sigprocmask(SIG_UNBLOCK, blockset, NULL);
		free(logStr);

		buf = malloc(bufSize * sizeof(char));
		sprintf(buf, "%d %d %d", totalBytes, totalWords, totalLines);

		if (fifoSend(writefd, buf, bufSize, 1) == -1) {
			free(buf);
			return -1;
		}
	} else {
		fprintf(stderr, "[-] Invalid worker command\n");
		return -1;
	}

	free(buf);
	freeResults(cmd, numberOfLines);
	return 0;
}


/* Replace a given keywords ":" with " C " since ':' is used as a log delimiter */
char *replaceColons(char *str) {
	int count = 0;
	int i;
	for (i = 0; i < strlen(str); i++) {
		if (str[i] == ':') {
			count++;
		}
	}

	int newSize = strlen(str) + 2 * count + 1;
	char *new = malloc(newSize * sizeof(char));
	int offset = 0;
	for (i = 0; i < strlen(str); i++) {
		if (str[i] == ':') {
			strcpy(new+offset, " C ");
			offset += 3;
		} else {
			new[offset] = str[i];
			offset++;
		}
	}

	new[newSize-1] = '\0';
	return new;
}


void writeLog(char *logStr, FILE *logfp) {
	// Create timestamp
	// (https://stackoverflow.com/questions/9596945/how-to-get-appropriate-timestamp-in-c-for-logs)
	time_t currTime = time(NULL);
	char *timestamp = asctime(localtime(&currTime));
	timestamp[strlen(timestamp) - 1] = '\0'; // Remove new-line
	// Convert ":" to ";" since ":" is used as a field seperator
	int i;
	for (i = 0; i < strlen(timestamp); i++) {
		if (timestamp[i] == ':') {
			timestamp[i] = ';';
		}
	}
	fprintf(logfp, "%s : %s\n", timestamp, logStr);
}


/* Read files inside the given directory and return the number of files found */
int getFilesInDir(char *dirPath, Trie *trie, int *totalBytes, int *totalWords, int *totalLines, FileList **list) {
	DIR *dir;
	struct dirent *direntp;

	if ((dir = opendir(dirPath)) == NULL) {
		perror("opendir");
		return -1;
	}

	// Read files
	while ((direntp = readdir(dir)) != NULL) {
		// Skip current directory and parent directory entries
		if (strcmp(direntp->d_name, ".") == 0 || strcmp(direntp->d_name, "..") == 0) {
			continue;
		}

		// Get statistics for every file
		int tempBytes = 0;
		int tempWords = 0;
		int tempLines = 0;

		// Create path to file
		char *filename = malloc((strlen(dirPath) + strlen(direntp->d_name) + 1) * sizeof(char));
		strcpy(filename, dirPath);
		strcat(filename, direntp->d_name);

		char **lines = readTextfile(filename, trie, &tempBytes, &tempWords, &tempLines);
		if (lines == NULL) {
			fprintf(stderr, "[-] Failed to read file %s\n", filename);
			free(filename);
			closedir(dir);
			return -1;
		}
		addFileNode(list, filename, lines, tempLines);

		// Add statistics to total
		*totalBytes += tempBytes;
		*totalWords += tempWords;
		*totalLines += tempLines;
	}

	closedir(dir);

	return 0;
}


void addFileNode(FileList **list, char *filename, char **lines, int numberOfLines) {
	// If the list is empty or we need to insert at the start,
	// create a new node and add it at the beginning
	if (*list == NULL || strcmp((*list)->filename, filename) > 0) {
		FileList *new = malloc(sizeof(FileList));
		new->filename = filename;
		new->lines = lines;
		new->numberOfLines = numberOfLines;

		new->next = *list;
		*list = new;
	} else { // Else, create a new node (file paths are unique, no need to check if a filename already exists)
		FileList *curr = *list;
		while (curr->next != NULL && strcmp(curr->next->filename, filename) < 0) {
			curr = curr->next;
		}

		// Create a new node at the right place of the sorted list
		FileList *new = malloc(sizeof(FileList));
		new->filename = filename;
		new->lines = lines;
		new->numberOfLines = numberOfLines;

		new->next = curr->next;
		curr->next = new;
	}
}


/* Return the contents of a file in lines */
char **getFileContents(FileList *head, char *filename) {
	if (head == NULL) {
		return NULL;
	}

	// Find node with the given filename
	while (head != NULL && strcmp(head->filename, filename) < 0) {
		head = head->next;
	}

	// Filename not found
	if (head == NULL || strcmp(head->filename, filename) != 0) {
		return NULL;
	}

	return head->lines;
}


void freeFileList(FileList *head) {
	if (head != NULL) {
		FileList *temp;
		while (head != NULL) {
			temp = head;
			head = head->next;
			free(temp->filename);
			int i;
			for (i = 0; i < temp->numberOfLines; i++) {
				free(temp->lines[i]);
			}
			free(temp->lines);
			free(temp);
		}
	}
}


void addKeyword(KeywordList *list, char *keyword) {
	// If the list is empty or we need to insert at the start,
	// create a new node and add it at the beginning
	if (list->head == NULL || strcmp(list->head->word, keyword) > 0) {
		KeywordNode *new = malloc(sizeof(KeywordNode));
		new->word = malloc((strlen(keyword) + 1) * sizeof(char));
		strcpy(new->word, keyword);
		list->numOfKeywords++;

		new->next = list->head;
		list->head = new;
	} else { // Else, check if the word has already been
			 // found. If not, create a new node
		KeywordNode *curr = list->head;
		if (strcmp(curr->word, keyword) == 0) {
			return;
		}

		while (curr->next != NULL && strcmp(curr->next->word, keyword) < 0) {
			curr = curr->next;
		}

		// If the word already exists, do nothing
		if (curr->next != NULL && strcmp(curr->next->word, keyword) == 0) {
			return;
		} else { // Create a new node at the right place of the sorted list
			KeywordNode *new = malloc(sizeof(KeywordNode));
			new->word = malloc((strlen(keyword) + 1) * sizeof(char));
			strcpy(new->word, keyword);
			list->numOfKeywords++;

			new->next = curr->next;
			curr->next = new;
		}
	}
}


void freeKeywordList(KeywordList *kwList) {
	if (kwList == NULL) {
		return;
	}

	KeywordNode *curr = kwList->head;
	KeywordNode *temp;
	while (curr != NULL) {
		temp = curr;
		curr = curr->next;
		free(temp->word);
		free(temp);
	}
}




void signal_handler(int sig, siginfo_t *siginfo, void *context) {
	switch(sig) {
	case SIGSEGV:
		printf("\n\nSEGFAULT\n");
		exit(0);
	case SIGALRM:
		if (getppid() == 1) { // Caught by init
			printf("[!] Job Executor killed!\n");
			raise(SIGTERM);
		}
		// Reset alarm
		alarm(PARENT_CHECK_TIME);
		break;
	case SIGUSR1:
		g_cmdReady = 1;
		break;
	case SIGUSR2:
		g_timedout = 1;
		break;
	case SIGHUP:
	case SIGINT:
	case SIGTERM:
		printf("[!] Worker exiting... (PID: %d)\n", getpid());
		destroy(g_trie);
		freeFileList(g_fileList);

		// Check who sent the signal
		//pid_t pid = siginfo->si_pid;

		// Notify parent about total keywords found
		// if the termination signal was sent from the JE
		/*if (pid == g_parentPid) {
			char buf[BUF_SIZE];
			sprintf(buf, "%d", g_kwList.numOfKeywords);
			fifoSend(g_writefd, buf, digits(g_kwList.numOfKeywords) + 1, 1);
		}*/

		freeKeywordList(&g_kwList);

		if (g_readfd != -1) {
			close(g_readfd);
		}
		if (g_writefd != -1) {
			close(g_writefd);
		}
		if (g_logfp != NULL) {
			fclose(g_logfp);
		}
		if (g_logname != NULL) {
			free(g_logname);
		}
		exit(0);
	}
}
