#ifndef COMM_H
#define COMM_H

#define EOF_BYTE   0x03
#define SEARCH_SEP 0x04 // Search results field seperator

char **fifoRecv(int, int *);
int fifoSend(int, char *, int, int);
void freeResults(char **, int);

#endif // COMM_H
