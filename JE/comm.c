#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "comm.h"

#define RESULTS_SIZE 32
#define BUF_SIZE   256

/* Read null terminated string from a file descriptor.
 * EOF_BYTE (0x03) is used to indicate the end of the input.
 * In case the other end is closed, the value of *numberOfResults becomes -2 */
char **fifoRecv(int readfd, int *numberOfResults) {
	// Allocate initial arrays and increase their size using realloc when needed
	char **results = malloc(RESULTS_SIZE * sizeof(char *));
	if (results == NULL) {
		perror("malloc");
		*numberOfResults = -1;
		return NULL;
	}
	int resultsSize = RESULTS_SIZE;

	char *buf = malloc(BUF_SIZE * sizeof(char));
	if (buf == NULL) {
		perror("malloc");
		free(results);
		*numberOfResults = -1;
		return NULL;
	}
	int bufSize = BUF_SIZE;

	int readOffset = 0; // Position of the buffer where we will write the bytes we read from the pipe
	int endOfText = 0;
	while(!endOfText) {
		int bytesRead = read(readfd, buf + readOffset, bufSize - readOffset);
		if (bytesRead == 0) { // End of file (other end closed)
			free(buf);
			*numberOfResults = -2;
			return NULL;
		}


		// Check if the other end stopped sending
		if (*(buf + readOffset + bytesRead - 1) == (char) EOF_BYTE) {
			*(buf + readOffset + bytesRead - 1) = '\0';
			endOfText = 1;
			bytesRead--;
		}

		// Resize buffer if needed
		while (readOffset + bytesRead >= bufSize) {
			bufSize *= 2;
			buf = realloc(buf, bufSize * sizeof(char));
			if (buf == NULL) {
				perror("realloc");
				*numberOfResults = -1;
				return NULL;
			}
		}
		readOffset += bytesRead;
	}


	// Split the buffer in strings
	int currLine = 0;
	int currPos = 0;
	int strSize;
	while (currPos < readOffset) {
		int i = currPos;
		while (buf[i] != '\0') {
			i++;
		}
		strSize = i - currPos + 1; // Include null-byte

		// Resize the results array if needed
		if (currLine >= resultsSize) {
			resultsSize *= 2;
			results = realloc(results, resultsSize * sizeof(char *));
			if (results == NULL) {
				perror("realloc");
				free(buf);
				*numberOfResults = -1;
				return NULL;
			}
		}

		results[currLine] = malloc(strSize * sizeof(char));
		if (results[currLine] == NULL) {
			perror("malloc");
			freeResults(results, currLine);
			free(buf);
			*numberOfResults = -1;
			return NULL;
		}
		strcpy(results[currLine], buf + currPos);
		currLine++;
		currPos += strSize;
	}

	free(buf);
	*numberOfResults = currLine;
	if (*numberOfResults == 0) {
		free(results);
		return NULL;
	} else {
		return results;
	}
}


/* Send a number of bytes through a FIFO. The transmission ends with EOF_BYTE. */
int fifoSend(int writefd, char *str, int bytesToWrite, int endOfTran) {
	if (bytesToWrite == 0 || str == NULL) {
		char eof = (char) EOF_BYTE;
		return write(writefd, &eof, 1);
	}

	char *new;
	int remainingBytes;
	// Create new string that includes EOF byte if it's the end of the transmission
	if (endOfTran) {
		new = malloc((bytesToWrite + 1) * sizeof(char));
		memcpy(new, str, bytesToWrite);
		new[(bytesToWrite)] = (char) EOF_BYTE;

		remainingBytes = bytesToWrite + 1;
	} else {
		new = str;
		remainingBytes = bytesToWrite;
	}

	int offset = 0;
	while (remainingBytes > 0) {
		int bytesWritten = write(writefd, new + offset, remainingBytes);
		if (bytesWritten == -1) {
			return -1;
		}
		remainingBytes -= bytesWritten;
		offset += bytesWritten;
	}

	if (new != str) {
		free(new);
	}

	return bytesToWrite;
}


/* Free results array up to and not including the given index */
void freeResults(char **results, int end) {
	int i;
	for (i = 0; i < end; i++) {
		free(results[i]);
	}
	free(results);
}
