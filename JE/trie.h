#ifndef TRIE_H
#define TRIE_H

#define MAXCOUNT 0
#define MINCOUNT 1

typedef struct lineList {
	int line;
	struct lineList *next;
} LineList;


/* Info about a file in which a word appears */
typedef struct {
	int fileFrequency; // Number of occurences in file
	LineList *lineList; // Sorted list of lines that contain the word
} FileInfo;


typedef struct postingsListNode {
	char *path;
	FileInfo fInfo;

	struct postingsListNode *next;
} PostingsNode;


typedef struct postingsList {
	PostingsNode *head;
} PostingsList;


typedef struct trieNode {
	char value;

	struct trieNode *next;
	struct trieNode *child;

	PostingsList *postings;
} TrieNode;


typedef struct trie {
	TrieNode *root;
} Trie;


void initialize(Trie **);
void trieInsert(Trie *, char *, char *, int);
PostingsList *mergePostingsLists(PostingsNode **, int);
char *getMaxMinCount(Trie *, char *, int *, int);
PostingsList *findPostingsList(Trie *, char *);
void freePostings(PostingsList *);
void destroy(Trie *);

#endif // TRIE_H
