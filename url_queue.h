#ifndef URL_QUEUE_H
#define URL_QUEUE_H

typedef struct urlInfo {
	char *url;

	struct urlInfo *next;
} UrlInfo;

typedef struct urlQueue {
	UrlInfo *front;
	UrlInfo *rear;
	int size;
} URLQueue;


void queueInit(URLQueue *);
int isEmpty(URLQueue *);
int queueInsert(URLQueue *, char *);
char *queueRemove(URLQueue *);
int queueExists(URLQueue *, char *);
void queueDestroy(URLQueue *);

#endif // URL_QUEUE_H
