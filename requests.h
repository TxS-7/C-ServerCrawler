#ifndef REQUESTS_H
#define REQUESTS_H

#define CODE_OK        200
#define CODE_NOT_FOUND 404
#define CODE_FORBIDDEN 403
#define CODE_BAD       400

char *parseRequest(char *);
char *createRequestHeaders(char *, char *);
char *createResponseHeaders(int, int);

#endif // REQUESTS_H
