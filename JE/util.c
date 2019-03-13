#define _XOPEN_SOURCE 500
#include <stdio.h> // remove
#include <unistd.h> // rmdir, stat, access
#include <sys/types.h>
#include <sys/stat.h>
#include <ftw.h> // nftw
#include "util.h"

int min(int x, int y) {
	return x < y ? x : y;
}


int Ceil(double x) {
	if ((int) x == x) {
		return x;
	} else {
		return ((int) x) + 1;
	}
}


/* Get number of digits of an integer */
int digits(int x) {
	if (x == 0) {
		return 1;
	}

	int dig = 0;
	while (x > 0) {
		dig++;
		x /= 10;
	}
	return dig;
}


int removeFun(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
	// remove() works for both files and directories
	if (remove(fpath) != 0) {
		perror("remove");
		return -1;
	}
	return 0;
}


/* Delete a directory and all its contents */
int removeDirectory(char *dirPath) {
	// Check if directory exists
	if (access(dirPath, F_OK) == -1) {
		return 0;
	}

	// Check if the path given is a directory
	struct stat dirStat;
	if (stat(dirPath, &dirStat) != 0) {
		perror("stat");
		return -1;
	}
	if (!S_ISDIR(dirStat.st_mode)) {
		return 0;
	}

	// Delete everything inside directory and the directory
	// (https://stackoverflow.com/questions/5467725/how-to-delete-a-directory-and-its-contents-in-posix-c)
	return nftw(dirPath, removeFun, 1, FTW_PHYS | FTW_DEPTH);
}
