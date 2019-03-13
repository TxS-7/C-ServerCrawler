#ifndef REQ_QUEUE_H
#define REQ_QUEUE_H

typedef struct request {
	char *filename;
	int client_sock;

	struct request *next;
} Request;

typedef struct requestQueue {
	Request *front;
	Request *rear;
	int size;
} RequestQueue;


void queueInit(RequestQueue *);
int isEmpty(RequestQueue *);
int isFull(RequestQueue *);
int queueInsert(RequestQueue *, char *, int);
int queueRemove(RequestQueue *, char **, int *);
void queueDestroy(RequestQueue *);

#endif // REQ_QUEUE_H
