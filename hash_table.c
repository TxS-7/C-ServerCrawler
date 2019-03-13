#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hash_table.h"


/* https://en.wikipedia.org/wiki/Jenkins_hash_function */
static unsigned long hash(char *key) {
	unsigned long hash = 0;
	int i;
	for (i = 0; i < strlen(key); i++) {
		hash += key[i];
		hash += (hash << 10);
		hash ^= (hash >> 6);
	}
	hash += (hash << 3);
	hash ^= (hash >> 11);
	hash += (hash << 15);

	return hash % TABLE_SIZE;
}


/* Initialize hash table buckets to NULL */
void HT_initialize(HashTable *table) {
	unsigned long i;
	for (i = 0; i < TABLE_SIZE; i++) {
		table->entries[i] = NULL;
	}
}


/* Insert new entry in Hash Table if the key doesn't already exist. */
int HT_insert(HashTable *table, char *key) {
	unsigned long index = hash(key);

	// Check if key already exists
	Entry *entry = table->entries[index];
	while (entry != NULL) {
		if (strcmp(entry->key, key) == 0) {
			return -1;
		}
		entry = entry->next;
	}

	// Create new entry
	Entry *newEntry = malloc(sizeof(Entry));
	if (newEntry == NULL) {
		perror("malloc");
		return -1;
	}
	newEntry->key = malloc((strlen(key) + 1) * sizeof(char));
	strcpy(newEntry->key, key);

	// Insert the new entry at the start of the chain of calculated index
	newEntry->next = table->entries[index];
	table->entries[index] = newEntry;
	return 0;
}


/* Free Hash Table memory */
void HT_destroy(HashTable *table) {
	unsigned long i;
	for (i = 0; i < TABLE_SIZE; i++) {
		if (table->entries[i] != NULL) {
			Entry *temp;
			Entry *curr = table->entries[i];
			while (curr != NULL) {
				temp = curr;
				curr = curr->next;
				free(temp->key);
				free(temp);
			}
		}
	}
}
