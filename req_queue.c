#include <stdlib.h>
#include <string.h>
#include "req_queue.h"

#define MAX_SIZE 32

void queueInit(RequestQueue *queue) {
	queue->front = NULL;
	queue->rear = NULL;
	queue->size = 0;
}


int isEmpty(RequestQueue *queue) {
	return queue->size == 0;
}


int isFull(RequestQueue *queue) {
	return queue->size == MAX_SIZE;
}


int queueInsert(RequestQueue *queue, char *filename, int client_sock) {
	if (isFull(queue)) {
		return -1;
	}

	Request *req = malloc(sizeof(Request));
	req->filename = malloc((strlen(filename) + 1) * sizeof(char));
	strcpy(req->filename, filename);
	req->client_sock = client_sock;
	req->next = NULL;

	(queue->size)++;

	// First element: change both front and rear
	if (queue->rear == NULL) {
		queue->rear = req;
		queue->front = req;
		return 0;
	} else {
		queue->rear->next = req;
		queue->rear = req;
		return 0;
	}
}


int queueRemove(RequestQueue *queue, char **filename, int *client_sock) {
	if (isEmpty(queue)) {
		return -1;
	}

	Request *req = queue->front;
	queue->front = queue->front->next;

	*filename = malloc((strlen(req->filename) + 1) * sizeof(char));
	strcpy(*filename, req->filename);
	*client_sock = req->client_sock;
	free(req->filename);
	free(req);

	(queue->size)--;

	// We removed the last element
	if (queue->front == NULL) {
		queue->rear = NULL;
	}

	return 0;
}


void queueDestroy(RequestQueue *queue) {
	if (isEmpty(queue)) {
		return;
	}

	Request *temp;
	while (queue->front != NULL) {
		temp = queue->front;
		queue->front = queue->front->next;
		free(temp->filename);
		free(temp);
	}
}
