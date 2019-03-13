#include <stdio.h>
#include <stdlib.h>
#include <string.h> // strlen
#include <ctype.h> // isblank
#include "docfile.h"

static int countLines(FILE *fp);


/* Read the file line by line and store directory paths in an array */
char **readDocfile(FILE *fp, int *numberOfDirs) {
	char *line = NULL;
	size_t len = 0;


	// Create the array of directory names
	*numberOfDirs = countLines(fp);
	if (*numberOfDirs <= 0) {
		return NULL;
	}
	char **directoryList = malloc(*numberOfDirs * sizeof(char *));
	if (directoryList == NULL) {
		perror("malloc");
		return NULL;
	}


	int currIndex = 0;
	while (getline(&line, &len, fp) != -1) {
		// Check only if line is empty (assuming no invalid directory names)
		if (strlen(line) == 1) { // Only new-line -> skip line (whitespace check below)
			free(line);
			len = 0;
			line = NULL;
			continue;
		}
		unsigned int i;
		int isEmpty = 1;
		for (i = 0; i < strlen(line) - 1; i++) { // Don't include new-line
			if (!isblank(line[i])) {
				isEmpty = 0;
				break;
			}
		}
		// If line is empty, skip it (check if line has only whitespaces)
		if (isEmpty) {
			free(line);
			len = 0;
			line = NULL;
			continue;
		}


		// Remove trailing whitespaces
		unsigned int end = strlen(line) - 1;
		while (isblank(line[end]) || line[end] == '\n') {
			end--;
		}
		line[end+1] = '\0';

		// Store the directory name in the directory list
		char *newline = malloc(sizeof(char) * (strlen(line) + 1));
		strcpy(newline, line);
		directoryList[currIndex] = newline;

		free(line);
		len = 0;
		line = NULL;
		currIndex++;
	}

	free(line);
	return directoryList;
}


/* Count number of non-empty lines in file */
int countLines(FILE *fp) {
	int lines = 0;
	int emptyLine = 1;
	while (!feof(fp)) {
		char ch = fgetc(fp);
		if (ch != '\n' && emptyLine && !isblank(ch)) {
			emptyLine = 0;
		} else if (ch == '\n' && !emptyLine) {
			lines++;
			emptyLine = 1;
		}
	}

	fseek(fp, 0, SEEK_SET); // Return to start of file
	return lines;
}


/* Free read directories up to and not including the given index */
void freeDirs(char **dirs, int end) {
	int i;
	for (i = 0; i < end; i++) {
		free(dirs[i]);
		dirs[i] = NULL;
	}
	free(dirs);
}
