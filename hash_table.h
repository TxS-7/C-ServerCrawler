#ifndef HASH_TABLE_H
#define HASH_TABLE_H

#define TABLE_SIZE 5077 // 5077 is a prime number

typedef struct entry {
	char *key;

	struct entry *next;
} Entry;

typedef struct table {
	Entry *entries[TABLE_SIZE];
} HashTable;

void HT_initialize(HashTable *);
int HT_insert(HashTable *, char *);
void HT_destroy(HashTable *);

#endif // HASH_TABLE_H
