#ifndef WORKER_H
#define WORKER_H

#define LOG_PATH      "./log/"
#define LOG_DIR_PERM      0700

typedef struct keywordNode {
	char *word;
	struct keywordNode *next;
} KeywordNode;

/* Sorted list of keywords searched */
typedef struct keywordList {
	KeywordNode *head;
	int numOfKeywords;
} KeywordList;

/* Name and contents of a file read by the worker */
typedef struct fileList {
	char *filename;
	char **lines;
	int numberOfLines;
	struct fileList *next;
} FileList;


int startWorker(char *, char *);

#endif // WORKER_H
