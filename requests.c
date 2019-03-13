#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "requests.h"

#define BUF_SIZE 128

static char *createTimeStamp(void);


/* Check if an HTTP request we received is in a valid format and return the file requested */
char *parseRequest(char *req) {
	char *reqsaveptr; // Used in strtok_r to split request in headers
	char *headersaveptr; // Used in strtok_r to get header name field and value

	// Check if request ends in \r\n\r\n
	int size = strlen(req);
	if (size <= 4 || strcmp(req + size - 4, "\r\n\r\n") != 0) {
		return NULL;
	}
	// Remove ening \r\n
	req[size-2] = '\0';


	// Get the first line
	char *reqInfo = strtok_r(req, "\n", &reqsaveptr);
	size = strlen(reqInfo);
	if (reqInfo == NULL || reqInfo[size-1] != '\r') {
		return NULL;
	}
	reqInfo[size-1] = '\0'; // Remove \r

	// Check if we have a GET request, if the HTTP/1.1 protocol is used
	// and if the page requested is valid
	char *type = strtok_r(reqInfo, " ", &headersaveptr);
	if (type == NULL || strcmp(type, "GET") != 0) {
		return NULL;
	}
	char *reqFile = strtok_r(NULL, " ", &headersaveptr);
	if (reqFile == NULL || reqFile[0] != '/') {
		return NULL;
	}
	char *prot = strtok_r(NULL, " ", &headersaveptr);
	if (prot == NULL || strcmp(prot, "HTTP/1.1") != 0) {
		return NULL;
	}
	char *extra = strtok_r(NULL, " ", &headersaveptr);
	if (extra != NULL) {
		return NULL;
	}


	// Check if every field is followed by \r\n and includes a ":"
	// and if there is a Host header included in the request
	int foundHost = 0;
	char *header;
	while ((header = strtok_r(NULL, "\n", &reqsaveptr)) != NULL) {
		int size = strlen(header);
		if (header[size-1] != '\r') {
			return NULL;
		}

		if (strchr(header, ':') == NULL) {
			return NULL;
		}


		char *field = strtok_r(header, ":", &headersaveptr);
		if (field == NULL) {
			return NULL;
		}
		if (strcmp(field, "Host") == 0) {
			foundHost = 1;

			// Check if Host header field is empty
			char *value = strtok_r(NULL, "\r\n", &headersaveptr);
			if (value == NULL) {
				return NULL;
			}
		}
	}
	if (!foundHost) {
		return NULL;
	}


	char *filename = malloc((strlen(reqFile) + 1) * sizeof(char));
	strcpy(filename, reqFile);
	return filename;
}


char *createRequestHeaders(char *host, char *filename) {
	char *headers = NULL;

	int size = 4 + strlen(filename) + 11 + 6 + strlen(host) + 4 + 1;

	headers = malloc(size * sizeof(char));
	if (headers == NULL) {
		perror("malloc");
		return NULL;
	}

	sprintf(headers, "GET %s HTTP/1.1\r\n", filename);
	strcat(headers, "Host: ");
	strcat(headers, host);
	strcat(headers, "\r\n\r\n");

	return headers;
}


char *createResponseHeaders(int code, int length) {
	char *headers = NULL;
	int size = 0;

	char *date = createTimeStamp();
	size += strlen(date) + strlen("Date: ") + 2;
	char *server = "Server: myhttpd/654.0.3\r\n";
	size += strlen(server);
	char contentLength[16];
	sprintf(contentLength, "Content-Length: %d\r\n", length);
	size += strlen(contentLength);
	char *contentType = "Content-Type: text/html\r\n";
	size += strlen(contentType);
	char *connection = "Connection: Closed\r\n";
	size += strlen(connection);
	char *info = NULL;

	if (code == CODE_OK) {
		info = "HTTP/1.1 200 OK\r\n";
	} else if (code == CODE_NOT_FOUND) {
		info = "HTTP/1.1 404 Not Found\r\n";
	} else if (code == CODE_FORBIDDEN) {
		info = "HTTP/1.1 403 Forbidden\r\n";
	} else if (code == CODE_BAD) {
		info = "HTTP/1.1 400 Bad Request\r\n";
	} else {
		free(date);
		return NULL;
	}

	size += strlen(info) + 2 + 1; // Include ending \r\n
	headers = malloc(size * sizeof(char));
	if (headers == NULL) {
		perror("malloc");
		free(date);
		return NULL;
	}
	strcpy(headers, info);
	strcat(headers, "Date: ");
	strcat(headers, date);
	strcat(headers, "\r\n");
	strcat(headers, server);
	strcat(headers, contentLength);
	strcat(headers, contentType);
	strcat(headers, connection);
	strcat(headers, "\r\n");

	free(date);
	return headers;
}


/* Create a timestamp for the HTTP Date header
 * https://stackoverflow.com/questions/7548759/generate-a-date-string-in-http-response-date-format-in-c */
char *createTimeStamp(void) {
	char *buf = malloc(BUF_SIZE * sizeof(char));
	time_t curr = time(NULL);
	struct tm curr_tm;
	// gmtime is not thread safe
	// gmtime_r should be thread safe
	gmtime_r(&curr, &curr_tm);
	strftime(buf, BUF_SIZE, "%a, %d %b %Y %H:%M:%S %Z", &curr_tm);
	return buf;
}
