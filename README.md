# Web Server and Crawler in C

# Description
## Web server
The web server is a multi-threaded HTTP server that accepts GET requests. It also accepts connections on a control port.
The commands for the control port are:
- STATS: to print statistics about requested pages and the uptime
- SHUTDOWN: to stop the server.
## Web crawler
The web crawler is a multi-threaded program that crawls a website downloading every page starting from a given URL and following any links it finds.
It also accepts connections on a control port. The commands for the control port are:
- STATS: to print statistics about the downloaded pages and the uptime
- SEARCH \<keyword-1> \<keyword-2> ... \<keyword-10>: Search for the given keywords in the downloaded pages and print the files and lines
in which they were found
- SHUTDOWN: to stop the crawler
## Web creator
The web creator bash script creates an example website consisting of directories and files in each directory. The files
are randomly created from a text file given as an argument to the script and include links to the other files in the same
or other directories


# Compile
$ make


# Execute
## Web Server
- $ ./myhttpd -p \<HTTP-port> -c \<command-port> -t \<number-of-threads> -d \<website-root-directory>
Example: ./myhttpd -p 8000 -c 9000 -t 10 -d website

## Web Crawler
- $ ./mycrawler -h \<remote-host/IP> -p \<remote-port> -c \<command-port> -t \<number-of-threads> -d \<destination-directory> \<starting-URL>
Example: ./mycrawler -h 127.0.0.1 -p 8000 -c 9001 -t 10 -d output http://127.0.0.1:8000/site1/page1_16165.html
("output" is an empty writable directory)

## Web Creator
- $ ./webcreator.sh \<destination-directory> \<text-file> \<number-of-directories> \<number-of-files-per-directory>
Example: ./webcreator.sh website pg164.txt 4 5
("website" is an empty writable directory)
