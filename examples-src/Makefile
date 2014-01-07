CC           = gcc -g
CFLAGS       = -Wall -I. -DSEATEST_EXIT_ON_FAIL -I..
LDFLAGS      = -L /opt/local/lib -L../ 
SYSLIBS      = -ldb

TSDB_LIB     = ../libtsdb.a


TARGETS      = tsdb-get \
		tsdb-set \
		tsdb-create

all: $(TARGETS)

%.o: %.c %.h
	${CC} ${CFLAGS} ${INCLUDE} -c $< -o $@

tsdb-%: tsdb_%.o $(TSDB_LIB)
	$(CC) $(LDFLAGS) tsdb_$*.o $(TSDB_LIB) $(SYSLIBS) -o $@

test-%: test_%.o $(TEST_LIBS)
	$(CC) $(LDFLAGS) test_$*.o $(TEST_LIBS) $(SYSLIBS) -o $@

clean:
	rm -f ${TARGETS} *.o *~ test-* tsdb-*

.SECONDARY: $(TEST_LIBS)
