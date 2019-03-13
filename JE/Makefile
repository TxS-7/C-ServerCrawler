OBJS  = util.o docfile.o textfile.o trie.o comm.o worker.o jobExecutor.o
CC    = gcc
FLAGS = -Wall -g3

all: $(OBJS)
	$(CC) -o jobExecutor $(OBJS)

jobExecutor.o: jobExecutor.c docfile.h worker.h worker_info.h comm.h util.h
	$(CC) $(FLAGS) -c jobExecutor.c

docfile.o: docfile.c docfile.h
	$(CC) $(FLAGS) -c docfile.c

worker.o: worker.c worker.h textfile.h trie.h comm.h util.h
	$(CC) $(FLAGS) -c worker.c

textfile.o: textfile.c textfile.h trie.h
	$(CC) $(FLAGS) -c textfile.c

comm.o: comm.c comm.h
	$(CC) $(FLAGS) -c comm.c

trie.o: trie.c trie.h
	$(CC) $(FLAGS) -c trie.c

util.o: util.c util.h
	$(CC) $(FLAGS) -c util.c

clean:
	rm -f $(OBJS)
