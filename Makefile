LIBGIT2_PREFIX ?= /opt/local

LISTBRANCH_OUT = listbranch
LISTBRANCH_OBJ = list-branches.o

LISTTAG_OUT = listtag
LISTTAG_OBJ = list-tags.o

GETALL_OUT = getall
GETALL_OBJ = config-getall.o

BRANCHFOR_OUT = branchfor
BRANCHFOR_OBJ = branches-with-commit.o

CFLAGS = -I$(LIBGIT2_PREFIX)/include -W -Wall -O0 -ggdb
LDFLAGS = -L$(LIBGIT2_PREFIX)/lib
LIBS = -lgit2

all: $(LISTBRANCH_OUT) $(LISTTAG_OUT) $(GETALL_OUT) $(BRANCHFOR_OUT)
	
clean:
	rm -f $(LISTBRANCH_OUT) $(LISTTAG_OUT) $(GETALL_OUT) $(BRANCHFOR_OUT)
	rm -f $(LISTBRANCH_OBJ) $(LISTTAG_OBJ) $(GETALL_OBJ) $(BRANCHFOR_OBJ)

$(LISTBRANCH_OUT): $(LISTBRANCH_OBJ)
	$(CC) $(LDFLAGS) -o $@ $+ $(LIBS)

$(LISTTAG_OUT): $(LISTTAG_OBJ)
	$(CC) $(LDFLAGS) -o $@ $+ $(LIBS)

$(GETALL_OUT): $(GETALL_OBJ)
	$(CC) $(LDFLAGS) -o $@ $+ $(LIBS)

$(BRANCHFOR_OUT): $(BRANCHFOR_OBJ)
	$(CC) $(LDFLAGS) -o $@ $+ $(LIBS)
