#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "trie.h"

#define CREATE_NEW   1
#define NOT_CREATE   0
#define BUF_SIZE   512

static TrieNode *getChild(TrieNode **, char, int);
static void updatePostingsList(PostingsList **, char *, int);
static void addLine(LineList **, int);

static void freeTrieList(TrieNode *);
static void freeLineList(LineList *);


// Initialize Trie root to NULL
void initialize(Trie **trie) {
	*trie = malloc(sizeof(Trie));
	(*trie)->root = NULL;
}


void trieInsert(Trie *trie, char *word, char *filename, int line) {
	TrieNode **curr = &(trie->root);
	TrieNode *found = NULL;

	unsigned int i;
	for (i = 0; i < strlen(word); i++) {
		// Get the child with character word[i].
		// If it doesn't exists it is created.
		found = getChild(curr, word[i], CREATE_NEW);
		curr = &(found->child);
	}

	// We reached the leaf node for this word.
	// Update the postings list
	updatePostingsList(&(found->postings), filename, line);
}


/* Find the child node with the given character.
 * If it doesn't exist, create a new node if flag is set.
 * Nodes are in alphabetical order to make searching
 * and insertion faster. */
TrieNode *getChild(TrieNode **head, char value, int createFlag) {
	if (*head == NULL || (*head)->value > value) {
		if (createFlag == NOT_CREATE) {
			return NULL;
		}
		TrieNode *new = malloc(sizeof(TrieNode));
		new->value = value;
		new->child = NULL;
		new->postings = NULL;

		new->next = *head;
		*head = new;

		return new;
	} else {
		if ((*head)->value == value) {
			return (*head);
		}

		TrieNode *curr = *head;
		while (curr->next != NULL && curr->next->value < value) {
			curr = curr->next;
		}

		// If the value already exists, just return the node found
		if (curr->next != NULL && curr->next->value == value) {
			return curr->next;
		} else { // Else, create a new node and place it at the right position in the sorted list of characters
			if (createFlag == NOT_CREATE) {
				return NULL;
			}
			TrieNode *new = malloc(sizeof(TrieNode));
			new->value = value;
			new->child = NULL;
			new->postings = NULL;

			new->next = curr->next;
			curr->next = new;

			return new;
		}
	}
}



/* Update the postings list and increase the word's document
 * frequency if necessary */
void updatePostingsList(PostingsList **list, char *filename, int line) {
	if (*list == NULL) {
		*list = malloc(sizeof(PostingsList));
		(*list)->head = NULL;
	}

	// If the postings list is empty or we need to insert at the start,
	// create a new node and add it at the beginning
	if ((*list)->head == NULL || strcmp((*list)->head->path, filename) > 0) {
		PostingsNode *new = malloc(sizeof(PostingsNode));
		new->path = filename;
		(new->fInfo).lineList = NULL;
		(new->fInfo).fileFrequency = 1;
		addLine(&((new->fInfo).lineList), line);

		new->next = (*list)->head;
		(*list)->head = new;
	} else { // Else, check if the word has already been
			 // found in the given file. If not, create a new node
		PostingsNode *curr = (*list)->head;
		if (strcmp(curr->path, filename) == 0) {
			((curr->fInfo).fileFrequency)++;
			addLine(&((curr->fInfo).lineList), line);
			return;
		}

		while (curr->next != NULL && strcmp(curr->next->path, filename) < 0) {
			curr = curr->next;
		}

		// If the file already exists, just increase the frequency and add the new line
		if (curr->next != NULL && strcmp(curr->next->path, filename) == 0) {
			((curr->next->fInfo).fileFrequency)++;
			addLine(&((curr->next->fInfo).lineList), line);
		} else { // Create a new node at the right place of the sorted list
			PostingsNode *new = malloc(sizeof(PostingsNode));
			new->path = filename;
			(new->fInfo).lineList = NULL;
			(new->fInfo).fileFrequency = 1;
			addLine(&((new->fInfo).lineList), line);

			new->next = curr->next;
			curr->next = new;
		}
	}
}


/* Find the given word's postings list in the Trie */
PostingsList *findPostingsList(Trie *trie, char *word) {
	TrieNode **curr = &(trie->root);
	TrieNode *found = NULL;

	unsigned int i;
	for (i = 0; i < strlen(word); i++) {
		// Get the child with character word[i].
		found = getChild(curr, word[i], NOT_CREATE);
		if (found == NULL) { // Word not found
			return NULL;
		}
		curr = &(found->child);
	}

	return found->postings;
}


void addLine(LineList **lineList, int line) {
	// If the list is empty or we need to insert at the start,
	// create a new node and add it at the beginning
	if (*lineList == NULL || (*lineList)->line > line) {
		LineList *new = malloc(sizeof(LineList));
		new->line = line;

		new->next = *lineList;
		*lineList = new;
	} else { // Else, check if the line has already been
			 // inserted. If not, create a new node
		LineList *curr = *lineList;
		if (curr->line == line) {
			return;
		}

		while (curr->next != NULL && curr->next->line < line) {
			curr = curr->next;
		}

		// If the line already exists, do nothing
		if (curr->next != NULL && curr->next->line == line) {
			return;
		} else { // Create a new node at the right place of the sorted list
			LineList *new = malloc(sizeof(LineList));
			new->line = line;

			new->next = curr->next;
			curr->next = new;
		}
	}
}


/* Merge postings lists by inserting them in a new postings list */
PostingsList *mergePostingsLists(PostingsNode **nodes, int numOfNodes) {
	PostingsList *newList = NULL;
	int i;
	for (i = 0; i < numOfNodes; i++) {
		PostingsNode *curr = nodes[i];
		while (curr != NULL) {
			LineList *lines = (curr->fInfo).lineList;
			while (lines != NULL) {
				updatePostingsList(&newList, curr->path, lines->line);
				lines = lines->next;
			}
			curr = curr->next;
		}
	}

	return newList;
}


char *getMaxMinCount(Trie *trie, char *word, int *count, int type) {
	PostingsList *postings = findPostingsList(trie, word);
	if (postings == NULL) { // Word not found
		*count = 0;
		return NULL;
	}

	PostingsNode *curr = postings->head;
	if (type == MAXCOUNT) {
		int max = (curr->fInfo).fileFrequency;
		char *maxFile = curr->path;
		curr = curr->next;
		while (curr != NULL) {
			if ((curr->fInfo).fileFrequency > max) {
				max = (curr->fInfo).fileFrequency;
				maxFile = curr->path;
			} else if ((curr->fInfo).fileFrequency == max) { // Sort by filename
				if (strcmp(curr->path, maxFile) < 0) {
					maxFile = curr->path;
				}
			}
			curr = curr->next;
		}
		*count = max;
		return maxFile;
	} else {
		int min = (curr->fInfo).fileFrequency;
		char *minFile = curr->path;
		curr = curr->next;
		while (curr != NULL) {
			if ((curr->fInfo).fileFrequency < min) {
				min = (curr->fInfo).fileFrequency;
				minFile = curr->path;
			} else if ((curr->fInfo).fileFrequency == min) { // Sort by filename
				if (strcmp(curr->path, minFile) < 0) {
					minFile = curr->path;
				}
			}
			curr = curr->next;
		}
		*count = min;
		return minFile;
	}
}




/* Free the memory allocated for the Trie structure
 * and its postings lists */
void destroy(Trie *trie) {
	if (trie == NULL) {
		return;
	}

	freeTrieList(trie->root);
	free(trie);
}


void freeTrieList(TrieNode *head) {
	TrieNode *temp;
	while (head != NULL) {
		if (head->child != NULL) {
			freeTrieList(head->child);
		}
		if (head->postings != NULL) {
			freePostings(head->postings);
		}

		temp = head;
		head = head->next;
		free(temp);
	}
}


void freePostings(PostingsList *list) {
	if (list != NULL) {
		PostingsNode *curr = list->head;
		PostingsNode *temp;

		while (curr != NULL) {
			temp = curr;
			curr = curr->next;
			freeLineList((temp->fInfo).lineList);
			free(temp);
		}

		free(list);
	}
}


void freeLineList(LineList *head) {
	if (head != NULL) {
		LineList *temp;
		while (head != NULL) {
			temp = head;
			head = head->next;
			free(temp);
		}
	}
}
