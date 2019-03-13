#ifndef WORKER_INFO_H
#define WORKER_INFO_H

#include <time.h>

typedef struct WorkerInfo {
	pid_t pid;

	char *readFifo;
	char *writeFifo;

	int readfd;
	int writefd;

	int start; // First directory index
	int end; // Last directory index

	int finished; // Worker returned command result
	time_t restartTime; // Last time when this worker was restarted
						// (Used to ignore results from queries when worker is killed)
} Worker;

#endif // WORKER_INFO_H
