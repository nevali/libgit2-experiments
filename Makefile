LIBGIT2_PREFIX ?= /opt/local

LISTBRANCH_OUT = listbranch
LISTBRANCH_OBJ = list-branches.o

CFLAGS = -I$(LIBGIT2_PREFIX)include -W -Wall -O0 -ggdb
LDFLAGS = -L$(LIBGIT2_PREFIX)/lib
LIBS = -lgit2

all: $(LISTBRANCH_OUT)
	
clean:
	rm -f $(LISTBRANCH_OUT)
	rm -f $(LISTBRANCH_OBJ)

$(LISTBRANCH_OUT): $(LISTBRANCH_OBJ)
	$(CC) $(LDFLAGS) -o $@ $+ $(LIBS)
