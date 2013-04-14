LIBGIT2_PREFIX ?= /opt/local

LISTBRANCH_OUT = listbranch
LISTBRANCH_OBJ = list-branches.o

LISTTAG_OUT = listtag
LISTTAG_OBJ = list-tags.o

CFLAGS = -I$(LIBGIT2_PREFIX)/include -W -Wall -O0 -ggdb
LDFLAGS = -L$(LIBGIT2_PREFIX)/lib
LIBS = -lgit2

all: $(LISTBRANCH_OUT) $(LISTTAG_OUT)
	
clean:
	rm -f $(LISTBRANCH_OUT) $(LISTTAG_OUT)
	rm -f $(LISTBRANCH_OBJ) $(LISTTAG_OBJ)

$(LISTBRANCH_OUT): $(LISTBRANCH_OBJ)
	$(CC) $(LDFLAGS) -o $@ $+ $(LIBS)

$(LISTTAG_OUT): $(LISTTAG_OBJ)
	$(CC) $(LDFLAGS) -o $@ $+ $(LIBS)
