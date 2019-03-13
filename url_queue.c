#include <stdlib.h>
#include <string.h>
#include "url_queue.h"


void queueInit(URLQueue *queue) {
	queue->front = NULL;
	queue->rear = NULL;
	queue->size = 0;
}


int isEmpty(URLQueue *queue) {
	return queue->size == 0;
}


int queueInsert(URLQueue *queue, char *url) {
	UrlInfo *info = malloc(sizeof(UrlInfo));
	info->url = malloc((strlen(url) + 1) * sizeof(char));
	strcpy(info->url, url);
	info->next = NULL;

	(queue->size)++;

	// First element: change both front and rear
	if (queue->rear == NULL) {
		queue->rear = info;
		queue->front = info;
		return 0;
	} else {
		queue->rear->next = info;
		queue->rear = info;
		return 0;
	}
}


char *queueRemove(URLQueue *queue) {
	if (isEmpty(queue)) {
		return NULL;
	}

	UrlInfo *info = queue->front;
	queue->front = queue->front->next;

	char *url = malloc((strlen(info->url) + 1) * sizeof(char));
	strcpy(url, info->url);
	free(info->url);
	free(info);

	(queue->size)--;

	// We removed the last element
	if (queue->front == NULL) {
		queue->rear = NULL;
	}

	return url;
}


/* Check if a URL already exists in the Queue */
int queueExists(URLQueue *queue, char *url) {
	if (isEmpty(queue)) {
		return 0;
	}
	
	UrlInfo *curr = queue->front;
	while (curr != NULL) {
		if (strcmp(curr->url, url) == 0) {
			return 1;
		}
		curr = curr->next;
	}

	return 0;
}


void queueDestroy(URLQueue *queue) {
	if (isEmpty(queue)) {
		return;
	}

	UrlInfo *temp;
	while (queue->front != NULL) {
		temp = queue->front;
		queue->front = queue->front->next;
		free(temp->url);
		free(temp);
	}
}
