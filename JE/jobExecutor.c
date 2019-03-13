#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h> // isblank
#include <unistd.h> // access
#include <signal.h> // signals
#include <sys/types.h> // mkfifo
#include <sys/select.h> // select
#include <sys/stat.h> // mkfifo
#include <sys/wait.h> // waitpid
#include <fcntl.h> // open, O_WRONLY, O_RDONLY
#include <errno.h> // errno
#include "util.h"
#include "docfile.h"
#include "worker_info.h"
#include "worker.h"
#include "comm.h"


#define FIFO_PATH   "./fifo/"
#define FIFO_DIR_PERM    0700
#define FIFO_PREFIX "worker_"
#define FIFO_PERMS       0600
#define BUF_SIZE          256
#define MAX_SEARCH_TERMS   10

#define MAXCOUNT 0
#define MINCOUNT 1


static int createWorkers(Worker *, int, char **, int);
static void restartWorker(Worker *, char **);
static int stopWorker(Worker *, int);
static void splitDirectories(Worker *, int, int, int);
static int sendDirectories(Worker *, char **);
static void readInput(Worker *workers, int);
static int commandSearch(Worker *workers, int, char **, int, int);
static char *commandMaxMin(Worker *, int, char *, int *, int);
static int commandWC(Worker *, int, int *, int *, int *);
static void signal_handler(int);
static void usage(char *);

// Global variables (used by signal handler)
static Worker *g_workers = NULL;
static int g_numberOfWorkers;
static char **g_directoryList = NULL;
static int g_numberOfDirs;

static volatile sig_atomic_t g_timedout = 0; // Used for search deadline

extern int errno;


int main(int argc, char *argv[]) {
	if (argc != 5) {
		usage(argv[0]);
		return -1;
	}

	// Setup signal handler
	struct sigaction act = {0};
	act.sa_handler = signal_handler;
	sigemptyset(&(act.sa_mask));
	sigaddset(&(act.sa_mask), SIGCHLD);
	sigaddset(&(act.sa_mask), SIGHUP);
	sigaddset(&(act.sa_mask), SIGINT);
	sigaddset(&(act.sa_mask), SIGTERM);
	sigaddset(&(act.sa_mask), SIGALRM);
	sigaction(SIGCHLD, &act, NULL);
	sigaction(SIGHUP, &act, NULL);
	sigaction(SIGINT, &act, NULL);
	sigaction(SIGTERM, &act, NULL);
	sigaction(SIGALRM, &act, NULL);

	// Ignore SIGPIPE
	struct sigaction ign = {0};
	sigemptyset(&(ign.sa_mask));
	sigaddset(&(ign.sa_mask), SIGPIPE);
	ign.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &ign, NULL);



	char *filename; // Docfile
	struct stat fileStat;

	// Parse arguments
	if (strcmp(argv[1], "-d") == 0) { // Read filename
		filename = argv[2];
		if (stat(filename, &fileStat) != 0) {
			perror("stat");
			return -2;
		}
		if (access(filename, F_OK) == -1 || !S_ISREG(fileStat.st_mode)) {
			fprintf(stderr, "[-] Invalid file %s\n", filename);
			return -2;
		}

		// Read number of workers
		if (strcmp(argv[3], "-w") != 0) {
			usage(argv[0]);
			return -1;
		}
		g_numberOfWorkers = atoi(argv[4]);
		if (g_numberOfWorkers <= 0) {
			fprintf(stderr, "[-] The number of workers must be a positive integer\n");
			return -3;
		}
	} else if (strcmp(argv[1], "-w") == 0) { // Read number of workers
		g_numberOfWorkers = atoi(argv[2]);
		if (g_numberOfWorkers <= 0) {
			fprintf(stderr, "[-] The number of workers must be a positive integer\n");
			return -3;
		}

		// Read filename
		if (strcmp(argv[3], "-d") != 0) {
			usage(argv[0]);
			return -1;
		}

		filename = argv[4];
		if (stat(filename, &fileStat) != 0) {
			perror("stat");
			return -2;
		}
		if (access(filename, F_OK) == -1 || !S_ISREG(fileStat.st_mode)) {
			fprintf(stderr, "[-] Invalid file %s\n", filename);
			return -2;
		}
	} else {
		usage(argv[0]);
		return -1;
	}



	printf("Job executor started with PID %d\n", getpid());

	// Remove previous FIFO directory and its contents if it already exists
	if (removeDirectory(FIFO_PATH) != 0) {
		fprintf(stderr, "[-] Failed to remove previous FIFO directory %s\n", FIFO_PATH);
		return -2;
	}
	// Create directory to store FIFOs
	if (mkdir(FIFO_PATH, FIFO_DIR_PERM) != 0) {
		perror("mkdir");
		return -2;
	}

	// Remove previous log directory and its contents if it already exists
	if (removeDirectory(LOG_PATH) != 0) {
		fprintf(stderr, "[-] Failed to remove previous log directory %s\n", LOG_PATH);
		return -2;
	}
	// Create directory for the workers to store their logs
	if (mkdir(LOG_PATH, LOG_DIR_PERM) != 0) {
		perror("mkdir");
		return -2;
	}


	// Read the document file
	FILE *fp = fopen(filename, "r");
	if (fp == NULL) {
		fprintf(stderr, "[-] Failed to open %s\n", filename);
		return -4;
	}

	printf("[*] Reading document file...\n");
	g_directoryList = readDocfile(fp, &g_numberOfDirs);
	if (g_directoryList == NULL) {
		fprintf(stderr, "[-] Error when reading docfile\n");
		fclose(fp);
		return -4;
	}
	fclose(fp);
	printf("[+] Finished reading document file\n");



	// Create the workers
	if (g_numberOfWorkers > g_numberOfDirs) {
		g_numberOfWorkers = g_numberOfDirs;
	}

	g_workers = malloc(g_numberOfWorkers * sizeof(Worker));
	if (g_workers == NULL) {
		perror("malloc (workers)");
		freeDirs(g_directoryList, g_numberOfDirs);
		return -5;
	}


	int activeWorkers = createWorkers(g_workers, g_numberOfWorkers, g_directoryList, g_numberOfDirs);
	if (activeWorkers <= 0) {
		fprintf(stderr, "[-] Error when creating the workers\n");
		free(g_workers);
		freeDirs(g_directoryList, g_numberOfDirs);
		return -6;
	}
	printf("[*] Workers ready (%d)\n\n\n", activeWorkers);


	splitDirectories(g_workers, g_numberOfWorkers, activeWorkers, g_numberOfDirs);

	// Set used to block SIGCHLD
	sigset_t blockset;
	sigemptyset(&blockset);
	sigaddset(&blockset, SIGCHLD);

	// Start sending the directories to the workers
	int i;
	for (i = 0; i < g_numberOfWorkers; i++) {
		if (g_workers[i].pid != -1) {
			if (sendDirectories(&g_workers[i], g_directoryList) < 0) {
				// If sending failed, ignore this worker
				fprintf(stderr, "[-] Failed to send directories %d - %d to worker %d\n", g_workers[i].start+1, g_workers[i].end, i+1);
				fprintf(stderr, "[!] Terminating failed worker\n");
				sigprocmask(SIG_SETMASK, &blockset, NULL);
				stopWorker(&g_workers[i], 0);
				sigprocmask(SIG_UNBLOCK, &blockset, NULL);
				activeWorkers--;
			}
		}
	}
	fflush(stdout);

	readInput(g_workers, g_numberOfWorkers);

	// We are about to terminate all children,
	// we don't need the SIGCHLD signal
	sigprocmask(SIG_SETMASK, &blockset, NULL);

	// Terminate all workers
	//printf("\n\n");
	for (i = 0; i < g_numberOfWorkers; i++) {
		if (g_workers[i].pid != -1) {
			//pid_t pid = g_workers[i].pid;
			//int result = stopWorker(&g_workers[i], 1);
			stopWorker(&g_workers[i], 0);
			//if (result >= 0) {
			//	printf("[+] Worker %d (%d) found [%d] keywords\n", i+1, pid, result);
			//}
		}
	}
	free(g_workers);
	g_workers = NULL;
	freeDirs(g_directoryList, g_numberOfDirs);
	g_directoryList = NULL;
	return 0;
}


/* Create FIFOs for all workers and create the worker processes using fork.
 * The function returns the number of workers that were created successfully */
int createWorkers(Worker *workers, int numberOfWorkers, char **dirList, int numOfDirs) {
	// Create the names for the named-pipes
	int workerIdLength = digits(numberOfWorkers);
	char *fifoRead = malloc((strlen(FIFO_PATH) + strlen(FIFO_PREFIX) + workerIdLength + 2 + 1) * sizeof(char));
	char *fifoWrite = malloc((strlen(FIFO_PATH) + strlen(FIFO_PREFIX) + workerIdLength + 2 + 1) * sizeof(char));
	sprintf(fifoRead, "%s%s", FIFO_PATH, FIFO_PREFIX);
	sprintf(fifoWrite, "%s%s", FIFO_PATH, FIFO_PREFIX);
	int idOffset = strlen(FIFO_PATH) + strlen(FIFO_PREFIX);

	int activeWorkers = numberOfWorkers;

	int i;
	for (i = 0; i < numberOfWorkers; i++) {
		// Create the first fifo for worker i
		// (Job Executor reading fifo)
		sprintf(fifoRead+idOffset, "%d.1", i+1);
		if (mkfifo(fifoRead, FIFO_PERMS) < 0) {
			perror("mkfifo : read");
			fprintf(stderr, "[-] Worker %d failed!\n", i+1);
			workers[i].pid = -1;
			activeWorkers--;
			continue;
		}

		// Create the second fifo for worker i
		// (Job Executor writing fifo)
		sprintf(fifoWrite+idOffset, "%d.2", i+1);
		if (mkfifo(fifoWrite, FIFO_PERMS) < 0) {
			perror("mkfifo : write");
			fprintf(stderr, "[-] Worker %d failed!\n", i+1);
			unlink(fifoRead);
			workers[i].pid = -1;
			activeWorkers--;
			continue;
		}



		pid_t pid = fork();
		if (pid > 0) { // Job Executor process
			// Add worker info
			workers[i].pid = pid;
			workers[i].readFifo = malloc((strlen(fifoRead)+1) * sizeof(char));
			strcpy(workers[i].readFifo, fifoRead);

			workers[i].writeFifo = malloc((strlen(fifoWrite)+1) * sizeof(char));
			strcpy(workers[i].writeFifo, fifoWrite);

			workers[i].readfd = -1;
			workers[i].writefd = -1;

			workers[i].restartTime = 0;

			printf("[+] Worker created with PID %d\n", pid);
		} else if (pid == 0) { // Child: Start worker
			int j;
			for (j = 0; j < i; j++) {
				Worker *worker = &workers[j];
				if (worker->readfd != -1) {
					close(worker->readfd);
				}
				free(worker->readFifo);
				worker->readFifo = NULL;

				if (worker->writefd != -1) {
					close(worker->writefd);
				}
				free(worker->writeFifo);
				worker->writeFifo = NULL;
			}
			freeDirs(dirList, numOfDirs);
			free(workers);

			// (Swap read and write fifos)
			int ret = 0;
			if (startWorker(fifoWrite, fifoRead) < 0) {
				fprintf(stderr, "[-] Failed to start worker %d\n", i+1);
				activeWorkers--;
				ret = -1;
			}
			exit(ret);
		} else { // A worker failed
			perror("fork");
			workers[i].pid = -1;
			activeWorkers--;
			unlink(fifoRead);
			unlink(fifoWrite);
		}
	}

	free(fifoRead);
	free(fifoWrite);
	return activeWorkers;
}


void restartWorker(Worker *worker, char **directoryList) {
	close(worker->readfd);
	worker->readfd = -1;
	close(worker->writefd);
	worker->writefd = -1;

	pid_t pid = fork();
	if (pid > 0) {
		worker->pid = pid;
		worker->restartTime = time(NULL);
		//printf("[+] Worker restarted with PID: %d\n", pid);

		// Set used to block SIGCHLD
		sigset_t blockset;
		sigemptyset(&blockset);
		sigaddset(&blockset, SIGCHLD);

		// Send directories
		if (sendDirectories(worker, directoryList) < 0) {
			// If sending failed, ignore this worker
			fprintf(stderr, "[-] Failed to send directories %d - %d to worker with PID %d\n", worker->start+1, worker->end, worker->pid);
			fprintf(stderr, "[!] Terminating failed worker\n");
			sigprocmask(SIG_SETMASK, &blockset, NULL);
			stopWorker(worker, 0);
			sigprocmask(SIG_UNBLOCK, &blockset, NULL);
		}
	} else if (pid == 0) {
		char *readFifo = malloc((strlen(worker->readFifo) + 1) * sizeof(char));
		strcpy(readFifo, worker->readFifo);
		char *writeFifo = malloc((strlen(worker->writeFifo) + 1) * sizeof(char));
		strcpy(writeFifo, worker->writeFifo);

		// Free JE memory
		int j;
		for (j = 0; j < g_numberOfWorkers; j++) {
			Worker *currWorker = &g_workers[j];
			if (currWorker->readfd != -1) {
				close(currWorker->readfd);
			}
			free(currWorker->readFifo);

			if (currWorker->writefd != -1) {
				close(currWorker->writefd);
			}
			free(currWorker->writeFifo);
		}
		freeDirs(g_directoryList, g_numberOfDirs);
		free(g_workers);

		if (startWorker(writeFifo, readFifo) < 0) {
			fprintf(stderr, "[-] Failed to restart worker\n");
			exit(-1);
		}
		exit(0);
	} else {
		perror("fork");
	}
}


int stopWorker(Worker *worker, int getResults) {
	kill(worker->pid, SIGTERM);

	int numberOfResults = -1;
	// Get number of search terms were found by the worker
	if (getResults) {
		char buf[BUF_SIZE];
		buf[BUF_SIZE-1] = '\0';
		if (read(worker->readfd, buf, BUF_SIZE - 1) == -1) {
			numberOfResults = -1;
		} else {
			numberOfResults = atoi(buf);
		}
	}


	worker->pid = -1;
	if (worker->readfd != -1) {
		close(worker->readfd);
	}
	unlink(worker->readFifo);
	free(worker->readFifo);
	worker->readFifo = NULL;

	if (worker->writefd != -1) {
		close(worker->writefd);
	}
	unlink(worker->writeFifo);
	free(worker->writeFifo);
	worker->writeFifo = NULL;

	return numberOfResults;
}


/* Store the list of directories for each worker in the worker info struct */
void splitDirectories(Worker *workers, int numberOfWorkers, int activeWorkers, int numberOfDirs) {
	int dirsPerWorker = numberOfDirs / activeWorkers;
	int currDir = 0;
	int i = 0;
	while (currDir < numberOfDirs && i < numberOfWorkers) {
		if (workers[i].pid != -1) {
			workers[i].start = currDir;
			if (i == numberOfWorkers - 1) {
				workers[i].end = numberOfDirs;
			} else {
				workers[i].end = currDir + dirsPerWorker;
			}
			currDir += dirsPerWorker;
		}
		i++;
	}
}


/* Open the named-pipes and send the list of directories to worker i through the JE writing named-pipe.
 * The directory names are seperated by null-bytes.
 * (By the time the JE calls this function the workers should have already opened their side of the FIFO
 * so that there is no blocking in the open call) */
int sendDirectories(Worker *worker, char **directoryList) {
	// Open the FIFO used for writing
	if ((worker->writefd = open(worker->writeFifo, O_WRONLY)) == -1) {
		return -1;
	}
	// Open the FIFO used for reading
	if ((worker->readfd = open(worker->readFifo, O_RDONLY)) == -1) {
		return -1;
	}


	// Calculate total buffer size
	int bufSize = 0;
	int i;
	for (i = worker->start; i < worker->end; i++) {
		bufSize += strlen(directoryList[i]) + 1;
	}
	char *buf = malloc(bufSize * sizeof(char));
	int offset = 0;

	// Copy directories to buffer
	for (i = worker->start; i < worker->end; i++) {
		strcpy(buf + offset, directoryList[i]);
		offset += strlen(directoryList[i]) + 1;
	}

	if (fifoSend(worker->writefd, buf, bufSize, 1) == -1) {
		return -1;
	}
	free(buf);

	return 0;
}


/* Keep reading commands from stdin until "/exit" is given */
void readInput(Worker *workers, int numberOfWorkers) {
	char *buf = NULL;
	size_t len = 0;
	while (1) {
		//printf("> ");

		if (getline(&buf, &len, stdin) == -1) {
			clearerr(stdin); // In case error is cause by signal interrupt
			free(buf);
			buf = NULL;
			len = 0;
			continue;
		}

		int textExists = 0;
		unsigned int i;
		for (i = 0; i < strlen(buf) - 1; i++) { // Skip new-line
			if (!isblank(buf[i])) {
				textExists = 1;
				break;
			}
		}
		if (!textExists) { // Only spaces given
			free(buf);
			buf = NULL;
			len = 0;
			continue;
		}

		if (buf[0] != '/') {
			//printf("[-] Invalid command (must start with '/')\n");
			free(buf);
			buf = NULL;
			len = 0;
			continue;
		}


		char *cmd = strtok(buf, " \t\n");
		/* ############# SEARCH ############### */
		if (strcmp(cmd, "/search") == 0) {
			char *terms[MAX_SEARCH_TERMS];
			// Read at most 10 arguments
			char *arg = strtok(NULL, " \t\n");
			if (arg == NULL || strcmp(arg, "-d") == 0) { // No search terms given
				//printf("[-] Invalid syntax\n");
			} else {
				int i = 0;
				while (i < 10 && arg != NULL && strcmp(arg, "-d") != 0) {
					terms[i] = arg;
					arg = strtok(NULL, " \t\n");
					i++;
				}

				// No deadline given
				if (arg == NULL || strcmp(arg, "-d") != 0) {
					//printf("[-] No deadline given\n");
					free(buf);
					buf = NULL;
					len = 0;
					continue;
				}

				// Read the deadline
				char *strDeadline = strtok(NULL, " \t\n");
				int deadline;
				if (strDeadline == NULL || (deadline = atoi(strDeadline)) <= 0) {
					//printf("[-] Invalid deadline given\n");
					free(buf);
					buf = NULL;
					len = 0;
					continue;
				}

				if (commandSearch(workers, numberOfWorkers, terms, i, deadline) < 0) {
					//printf("[-] Command failed\n");
					free(buf);
					break;
				}
			}
		/* ########## MAXCOUNT ########### */
		} else if (strcmp(cmd, "/maxcount") == 0) {
			char *arg = strtok(NULL, " \t\n");
			if (arg == NULL) {
				printf("[-] Invalid syntax\n");
			} else {
				int count;
				char *filename = commandMaxMin(workers, numberOfWorkers, arg, &count, MAXCOUNT);
				if (filename == NULL && count < 0) {
					printf("[-] Command failed\n");
					free(buf);
					break;
				} else if (filename == NULL && count == 0) {
					printf("[+] Word not found\n\n");
				} else {
					printf("[+] File with maximum occurences (%d): %s\n\n", count, filename);
					free(filename);
				}
			}
		/* ########## MINCOUNT ################ */
		} else if (strcmp(cmd, "/mincount") == 0) {
			char *arg = strtok(NULL, " \t\n");
			if (arg == NULL) {
				printf("[-] Invalid syntax\n");
			} else {
				int count;
				char *filename = commandMaxMin(workers, numberOfWorkers, arg, &count, MINCOUNT);
				if (filename == NULL && count < 0) {
					printf("[-] Command failed\n");
					free(buf);
					break;
				} else if (filename == NULL && count == 0) {
					printf("[+] Word not found\n\n");
				} else {
					printf("[+] File with minimum occurences (%d): %s\n\n", count, filename);
					free(filename);
				}
			}
		/* ############# WC ############### */
		} else if (strcmp(cmd, "/wc") == 0) {
			int totalBytes = 0;
			int totalWords = 0;
			int totalLines = 0;
			if (commandWC(workers, numberOfWorkers, &totalBytes, &totalWords, &totalLines) == 0) {
				printf("[+] Bytes: %d   |   Words: %d   |   Lines: %d\n\n", totalBytes, totalWords, totalLines);
			} else {
				printf("[-] Command failed\n");
				free(buf);
				break;
			}
		/* ############ EXIT ############## */
		} else if (strcmp(cmd, "/exit") == 0) {
			printf("Exiting program...\n");
			free(buf);
			break;
		} else {
			//printf("[-] Invalid command\n");
		}

		free(buf);
		buf = NULL;
		len = 0;
	}
}


int commandSearch(Worker *workers, int numberOfWorkers, char **terms, int numberOfTerms, int deadline) {
	time_t queryTime = time(NULL);

	// Notify workers to read command and send the command
	int i;
	int activeWorkers = 0;
	for (i = 0; i < numberOfWorkers; i++) {
		if (workers[i].pid != -1) {
			kill(workers[i].pid, SIGUSR1);
			workers[i].finished = 0;
			activeWorkers++;
		}
	}

	char cmd[11];
	strcpy(cmd, "CMD:SEARCH");

	int argSize = 0;
	for (i = 0; i < numberOfTerms; i++) {
		argSize += strlen(terms[i]) + 1;
	}

	char *arg = malloc(argSize * sizeof(char));
	int offset = 0;
	for (i = 0; i < numberOfTerms; i++) {
		strcpy(arg + offset, terms[i]);
		offset += strlen(terms[i]) + 1;
	}

	for (i = 0; i < numberOfWorkers; i++) {
		if (workers[i].pid != -1) {
			if (fifoSend(workers[i].writefd, cmd, 11, 0) == -1) {
				free(arg);
				return -1;
			}
			if (fifoSend(workers[i].writefd, arg, argSize, 1) == -1) {
				free(arg);
				return -1;
			}
		}
	}
	free(arg);

	// Set alarm after deadline seconds
	alarm(deadline);

	int resultsFound = 0;
	int sentResults = 0;
	char ***totalResults = malloc(activeWorkers * sizeof(char **)); // Array of results from all workers
	int *resultsCount = malloc(activeWorkers * sizeof(int)); // Number of results returned from every worker

	// Seperator used in strtok
	char seperator[2];
	seperator[0] = (char) SEARCH_SEP;
	seperator[1] = '\0';


	// Wait for result from workers
	int remainingWorkers = activeWorkers;
	int currWorker = 0; // Used to check restarted workers
	while (remainingWorkers > 0) {
		fd_set fds;
		FD_ZERO(&fds);
		int maxfd = -1;

		int j;
		for (j = 0; j < numberOfWorkers; j++) {
			if (workers[j].pid != -1 && !workers[j].finished) {
				if (workers[j].readfd > maxfd) {
					maxfd = workers[j].readfd;
				}
				FD_SET(workers[j].readfd, &fds);
			}
		}

		int ret;
		if (g_timedout || ((ret = select(maxfd + 1, &fds, NULL, NULL, NULL)) == -1 && errno == EINTR && g_timedout)) { // Received alarm signal
			// Tell remaining workers to just send an empty result
			// after they finish searching for results (SIGUSR2)
			int k;
			for (k = 0; k < numberOfWorkers; k++) {
				if (workers[k].pid != -1 && !workers[k].finished) {
					kill(workers[k].pid, SIGUSR2);
				}
			}

			// Print the results we got so far and ignore the rest
			printf("[*] Received results from %d / %d workers\n\n", currWorker, activeWorkers);
			if (!resultsFound) {
				printf("[+] No results found\n\n");
			} else {
				int i;
				for (i = 0; i < currWorker; i++) {
					// The results are in the form:
					// <filename><SEARCH_SEP><line number><SEARCH_SEP><line contents><NULL BYTE>

					int k;
					for (k = 0; k < resultsCount[i]; k++) {
						char *filename = strtok(totalResults[i][k], seperator);
						char *lineNumberStr = strtok(NULL, seperator);
						int lineNumber = atoi(lineNumberStr);
						char *lineContents = strtok(NULL, seperator);
						printf("%s : %d : %s\n\n\n", filename, lineNumber, lineContents);
					}
				}
			}
			g_timedout = 0;
			sentResults = 1;
			continue;
		} else if (ret == -1 && errno == EINTR && !g_timedout) { // Check if we got SIGCHLD
			// If a worker that hadn't finished was terminated, don't wait for results from him
			int terminatedWorkers = 0;
			int i;
			for (i = 0; i < numberOfWorkers; i++) {
				if (workers[i].restartTime > queryTime && !workers[i].finished) {
					terminatedWorkers++;
					workers[i].finished = 1;
					activeWorkers--;
				}
			}

			if (terminatedWorkers > 0) {
				printf("[!] Ignoring results from %d terminated workers\n", terminatedWorkers);
				remainingWorkers -= terminatedWorkers;
			}
			continue;
		} else if (ret == -1 && (errno != EINTR || !g_timedout)) { // Error occured or different signal received
			int i;
			for (i = 0; i < currWorker; i++) {
				freeResults(totalResults[i], resultsCount[i]);
			}
			free(totalResults);
			free(resultsCount);
			return -1;
		}

		// Check which fd became available for reading
		for (j = 0; j < numberOfWorkers; j++) {
			if (FD_ISSET(workers[j].readfd, &fds)) {
				workers[j].finished = 1;
				remainingWorkers--;

				int numOfResults;
				char **results = fifoRecv(workers[j].readfd, &numOfResults);
				if (numOfResults == -2) { // We probably got EOF when child terminated and closed the FIFO
					printf("[!] Ignoring results from terminated worker\n");
					activeWorkers--;
					continue;
				} else if (numOfResults < 0) {
					return -1;
				}

				totalResults[currWorker] = results;
				resultsCount[currWorker] = numOfResults;
				currWorker++;
				if (numOfResults != 0) {
					resultsFound = 1;
				}
			}
		}
	}


	// All results ready before deadline
	if (!sentResults) {
		// Cancel pending alarm signal
		alarm(0);
		g_timedout = 0; // Watch out for alarm signal before we call alarm(0)
		printf("[*] Received results from %d / %d workers\n\n", activeWorkers, activeWorkers);
		if (!resultsFound) {
			printf("[+] No results found\n\n");
		} else {
			int i;
			for (i = 0; i < activeWorkers; i++) {
				// The results are in the form:
				// <filename><SEARCH_SEP><line number><SEARCH_SEP><line contents><NULL BYTE>

				int k;
				for (k = 0; k < resultsCount[i]; k++) {
					char *filename = strtok(totalResults[i][k], seperator);
					char *lineNumberStr = strtok(NULL, seperator);
					int lineNumber = atoi(lineNumberStr);
					char *lineContents = strtok(NULL, seperator);
					printf("%s : %d : %s\n\n\n", filename, lineNumber, lineContents);
				}
			}
		}
	}

	for (i = 0; i < activeWorkers; i++) {
		freeResults(totalResults[i], resultsCount[i]);
	}
	free(totalResults);
	free(resultsCount);
	fflush(stdout);
	return 0;
}


char *commandMaxMin(Worker *workers, int numberOfWorkers, char *word, int *count, int type) {
	time_t queryTime = time(NULL);
	*count = -1; // Used as error

	// Notify workers to read command and send the command
	int i;
	int activeWorkers = 0;
	for (i = 0; i < numberOfWorkers; i++) {
		if (workers[i].pid != -1) {
			kill(workers[i].pid, SIGUSR1);
			workers[i].finished = 0;
			activeWorkers++;
		}
	}

	char cmd[13];
	if (type == MAXCOUNT) {
		strcpy(cmd, "CMD:MAXCOUNT");
	} else {
		strcpy(cmd, "CMD:MINCOUNT");
	}

	int argSize = strlen(word) + 1;
	char *arg = malloc(argSize * sizeof(char));
	strcpy(arg, word);

	for (i = 0; i < numberOfWorkers; i++) {
		if (workers[i].pid != -1) {
			if (fifoSend(workers[i].writefd, cmd, 13, 0) == -1) {
				free(arg);
				return NULL;
			}
			if (fifoSend(workers[i].writefd, arg, argSize, 1) == -1) {
				free(arg);
				return NULL;
			}
		}
	}
	free(arg);

	*count = 0;
	int currValue;
	char *currFilename = NULL;

	// Wait for result from workers
	int remainingWorkers = activeWorkers;
	int firstWorkerFound = 0; // Used to set initial max/min value
	while (remainingWorkers > 0) {
		fd_set fds;
		FD_ZERO(&fds);
		int maxfd = -1;

		int j;
		for (j = 0; j < numberOfWorkers; j++) {
			if (workers[j].pid != -1 && !workers[j].finished) {
				if (workers[j].readfd > maxfd) {
					maxfd = workers[j].readfd;
				}
				FD_SET(workers[j].readfd, &fds);
			}
		}

		if (select(maxfd + 1, &fds, NULL, NULL, NULL) == -1) {
			if (errno != EINTR) {
				return NULL;
			} else { // Check if we got SIGCHLD
				// If a worker that hadn't finished was terminated, don't wait for results from him
				int terminatedWorkers = 0;
				int i;
				for (i = 0; i < numberOfWorkers; i++) {
					if (workers[i].restartTime > queryTime && !workers[i].finished) {
						terminatedWorkers++;
						workers[i].finished = 1;
						activeWorkers--;
					}
				}

				if (terminatedWorkers > 0) {
					printf("[!] Ignoring results from %d terminated workers\n", terminatedWorkers);
					remainingWorkers -= terminatedWorkers;
				}
				continue;
			}
		}

		// Check which fd became available for reading
		for (j = 0; j < numberOfWorkers; j++) {
			if (FD_ISSET(workers[j].readfd, &fds)) {
				workers[j].finished = 1;
				remainingWorkers--;

				int numOfResults;
				char **results = fifoRecv(workers[j].readfd, &numOfResults);
				if (numOfResults == -2) { // We probably got EOF when child terminated and closed the FIFO
					activeWorkers--;
					printf("[!] Ignoring results from terminated worker\n");
					continue;
				} else if (numOfResults < 0) {
					*count = -1;
					return NULL;
				}

				// Read space seperated results
				char *filename = strtok(results[0], " ");
				char *occStr = strtok(NULL, " ");
				int occurences = atoi(occStr);
				if (occurences > 0) {
					if (!firstWorkerFound) {
						currValue = occurences;
						if (currFilename != NULL) {
							free(currFilename);
						}
						currFilename = malloc((strlen(filename) + 1) * sizeof(char));
						strcpy(currFilename, filename);
						firstWorkerFound = 1;
					} else {
						if (type == MAXCOUNT) {
							if (occurences > currValue) {
								currValue = occurences;
								if (currFilename != NULL) {
									free(currFilename);
								}
								currFilename = malloc((strlen(filename) + 1) * sizeof(char));
								strcpy(currFilename, filename);
							} else if (occurences == currValue) { // Sort by filename
								if (strcmp(filename, currFilename) < 0) {
									if (currFilename != NULL) {
										free(currFilename);
									}
									currFilename = malloc((strlen(filename) + 1) * sizeof(char));
									strcpy(currFilename, filename);
								}
							}
						} else {
							if (occurences < currValue) {
								currValue = occurences;
								if (currFilename != NULL) {
									free(currFilename);
								}
								currFilename = malloc((strlen(filename) + 1) * sizeof(char));
								strcpy(currFilename, filename);
							} else if (occurences == currValue) { // Sort by filename
								if (strcmp(filename, currFilename) < 0) {
									if (currFilename != NULL) {
										free(currFilename);
									}
									currFilename = malloc((strlen(filename) + 1) * sizeof(char));
									strcpy(currFilename, filename);
								}
							}
						}
					}
				}
				freeResults(results, numOfResults);
			}
		}
	}

	if (currFilename != NULL) {
		*count = currValue;
	}
	return currFilename;
}


int commandWC(Worker *workers, int numberOfWorkers, int *totalBytes, int *totalWords, int *totalLines) {
	time_t queryTime = time(NULL);
	*totalBytes = 0;
	*totalWords = 0;
	*totalLines = 0;

	// Notify workers to read command and send the the command
	int i;
	int activeWorkers = 0;
	for (i = 0; i < numberOfWorkers; i++) {
		if (workers[i].pid != -1) {
			kill(workers[i].pid, SIGUSR1);
			workers[i].finished = 0;
			activeWorkers++;
		}
	}

	char cmd[7] = "CMD:WC";
	for (i = 0; i < numberOfWorkers; i++) {
		if (workers[i].pid != -1) {
			if (fifoSend(workers[i].writefd, cmd, 7, 1) == -1) {
				return -1;
			}
		}
	}

	// Wait for result from workers
	int remainingWorkers = activeWorkers;
	while (remainingWorkers > 0) {
		fd_set fds;
		FD_ZERO(&fds);
		int maxfd = -1;

		int j;
		for (j = 0; j < numberOfWorkers; j++) {
			if (workers[j].pid != -1 && !workers[j].finished) {
				if (workers[j].readfd > maxfd) {
					maxfd = workers[j].readfd;
				}
				FD_SET(workers[j].readfd, &fds);
			}
		}

		if (select(maxfd + 1, &fds, NULL, NULL, NULL) == -1) {
			if (errno != EINTR) {
				return -1;
			} else { // Check if we got SIGCHLD
				// If a worker that hadn't finished was terminated, don't wait for results from him
				int terminatedWorkers = 0;
				int i;
				for (i = 0; i < numberOfWorkers; i++) {
					if (workers[i].restartTime > queryTime && !workers[i].finished) {
						terminatedWorkers++;
						workers[i].finished = 1;
						activeWorkers--;
					}
				}

				if (terminatedWorkers > 0) {
					printf("[!] Ignoring results from %d terminated workers\n", terminatedWorkers);
					remainingWorkers -= terminatedWorkers;
				}
				continue;
			}
		}


		// Check which fd became available for reading
		for (j = 0; j < numberOfWorkers; j++) {
			if (FD_ISSET(workers[j].readfd, &fds)) {
				workers[j].finished = 1;
				remainingWorkers--;

				int numOfResults;
				char **results = fifoRecv(workers[j].readfd, &numOfResults);
				if (numOfResults == -2) { // We probably got EOF when child terminated and closed the FIFO
					activeWorkers--;
					printf("[!] Ignoring results from terminated worker\n");
					continue;
				} else if (numOfResults < 0) {
					return -1;
				}

				// Read space seperated results
				char *bytesStr = strtok(results[0], " ");
				*totalBytes += atoi(bytesStr);
				char *wordsStr = strtok(NULL, " ");
				*totalWords += atoi(wordsStr);
				char *linesStr = strtok(NULL, " ");
				*totalLines += atoi(linesStr);

				freeResults(results, numOfResults);
			}
		}
	}

	return 0;
}




void signal_handler(int sig) {
	int i;
	switch(sig) {
	case SIGALRM:
		g_timedout = 1;
		break;
	case SIGCHLD: ;
		// Restart terminated worker
		int status = 0;
		pid_t pid = waitpid(-1, &status, WNOHANG); // Get pid of terminated child (if more children quit, they will be handled later)
		if (pid == 0) { // False alarm, no child quit
			return;
		} else if (pid == -1) {
			perror("waitpid");
			exit(-10);
		}

		//printf("[!] Worker with PID %d terminated\n[*] Restarting worker\n", pid);

		// Get worker index using pid
		for (i = 0; i < g_numberOfWorkers; i++) {
			if (g_workers[i].pid == pid) {
				break;
			}
		}
		// Restart worker
		restartWorker(&g_workers[i], g_directoryList);
		break;
	case SIGHUP:
	case SIGINT:
	case SIGTERM: ;
		//printf("\n! TERMINATION SIGNAL !\n\n");
		// Terminate all workers
		for (i = 0; i < g_numberOfWorkers; i++) {
			if (g_workers[i].pid != -1) {
				stopWorker(&g_workers[i], 0);
			}
		}

		if (g_directoryList != NULL) {
			freeDirs(g_directoryList, g_numberOfDirs);
		}
		if (g_workers != NULL) {
			free(g_workers);
		}
		exit(-100);
	}
}


/* Print usage message */
void usage(char *programName) {
	printf("Usage: %s -d <docfile> -w <numWorkers>\n", programName);
}
