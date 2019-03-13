HTTPD_OBJS   = req_queue.o requests.o myhttpd.o
CRAWLER_OBJS = util.o hash_table.o url_queue.o requests.o mycrawler.o
CC           = gcc
FLAGS        = -Wall -g3

all: myhttpd mycrawler jobExecutor

myhttpd: $(HTTPD_OBJS)
	$(CC) -o myhttpd -pthread $(HTTPD_OBJS)

myhttpd.o: myhttpd.c req_queue.h requests.h
	$(CC) $(FLAGS) -pthread -c myhttpd.c

req_queue.o: req_queue.c req_queue.h
	$(CC) $(FLAGS) -c req_queue.c


mycrawler: $(CRAWLER_OBJS)
	$(CC) -o mycrawler -pthread $(CRAWLER_OBJS)

mycrawler.o: mycrawler.c hash_table.h url_queue.h util.h requests.h
	$(CC) $(FLAGS) -pthread -c mycrawler.c

url_queue.o: url_queue.c url_queue.h
	$(CC) $(FLAGS) -c url_queue.c

hash_table.o: hash_table.c hash_table.h
	$(CC) $(FLAGS) -c hash_table.c

util.o: util.c util.h
	$(CC) $(FLAGS) -c util.c


requests.o: requests.c requests.h
	$(CC) $(FLAGS) -c requests.c


jobExecutor:
	cd JE && $(MAKE)


clean:
	rm -f $(HTTPD_OBJS) $(CRAWLER_OBJS)
	cd JE && $(MAKE) clean
