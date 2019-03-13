#include <stdio.h>
#include <stdlib.h>
#include <string.h> // strlen
#include <ctype.h> // isblank
#include "textfile.h"
#include "trie.h"

static int countLines(FILE *);
static void freeLines(char **, unsigned int);


/* Read the file line by line and store the words in the trie and the lines in an array */
char **readTextfile(char *filename, Trie *trie, int *totalBytes, int *totalWords, int *totalLines) {
	// Open the file for reading
	FILE *fp = fopen(filename, "r");

	char *line = NULL;
	size_t len = 0;
	*totalBytes = 0;
	*totalWords = 0;


	// Create the array of lines
	*totalLines = countLines(fp);
	if (*totalLines <= 0) {
		fclose(fp);
		return NULL;
	}
	char **lines = malloc(*totalLines * sizeof(char *));
	if (lines == NULL) {
		perror("malloc");
		fclose(fp);
		return NULL;
	}

	int currIndex = 0;
	while (getline(&line, &len, fp) != -1) {
		// Remove the HTML tags from the text (anything between < and >)
		unsigned int i = 0;
		unsigned int j = 0;
		while (i < strlen(line)) {
			while (line[i] == '<') {
				while (line[i] != '>'&& i < strlen(line)) {
					i++;
				}
				i++; // Skip the >
			}
			line[j++] = line[i++];
		}
		line[j] = '\0';

		// Store number of bytes (including new-line)
		*totalBytes += strlen(line);

		// Check if read an empty line (only whitespace)
		int isEmpty = 1;
		for (i = 0; i < strlen(line) - 1; i++) { // Don't include new-line
			if (!isblank(line[i])) {
				isEmpty = 0;
				break;
			}
		}

		// Remove new-line
		unsigned int end = strlen(line) - 1;
		line[end] = '\0';

		// Copy the line to a new string to avoid changes from strtok
		// and be able to free it later
		char *newLine = malloc(sizeof(char) * (strlen(line) + 1));
		if (newLine == NULL) {
			perror("malloc");
			freeLines(lines, currIndex);
			free(line);
			fclose(fp);
			return NULL;
		}

		strcpy(newLine, line);
		// Store line in array
		lines[currIndex] = newLine;

		// If the line contains at least one non-whitespace character
		// read its words
		if (!isEmpty) {
			char *word = strtok(line, " \t\n");
			while (word != NULL) {
				// Insert word in Trie along with textId
				trieInsert(trie, word, filename, currIndex);
				// Increase number of total words
				(*totalWords)++;
				word = strtok(NULL, " \t\n");
			}
		}

		free(line);
		len = 0;
		line = NULL;
		currIndex++;
	}

	free(line);
	fclose(fp);
	return lines;
}


/* Count number of lines in file */
int countLines(FILE *fp) {
	int lines = 0;
	while (!feof(fp)) {
		char ch = fgetc(fp);
		if (ch == '\n') {
			lines++;
		}
	}

	fseek(fp, 0, SEEK_SET); // Return to start of file
	return lines;
}


/* Free read lines up to and not including the given index */
void freeLines(char **lines, unsigned int end) {
	unsigned int i;
	for (i = 0; i < end; i++) {
		free(lines[i]);
	}
	free(lines);
}
