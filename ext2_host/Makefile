CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -O1 -g -DEXT2_HOST_TEST 
LDFLAGS = 

.PHONY: all clean test

all: ext2_test

ext2_test: ext2.c ext2_test.c ext2.h
	$(CC) $(CFLAGS) -o $@ ext2.c ext2_test.c $(LDFLAGS)

ext2.img: make_test_img.sh
	./make_test_img.sh

test: ext2_test ext2.img
	./ext2_test ext2.img

clean:
	rm -f ext2_test ext2.img
	rm -rf mnt_ext2_test
