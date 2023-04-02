CC = gcc
CFLAGS = -ggdb3 -Wall

build: aws

aws: aws.o sock_util.o http_parser.o
	$(CC) $(CFLAGS) -o $@ $^ -laio

aws.o: aws.c
	$(CC) $(CFLAGS) -c $<
sock_util.o: sock_util.c
	$(CC) $(CFLAGS) -c $<

http_parser.o: http_parser.c
	$(CC) $(CFLAGS) -c $<

.PHONY: clean

clean:
	rm -f *.o aws