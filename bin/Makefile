CC           = gcc -g
CFLAGS       = -Wall -I. -DSEATEST_EXIT_ON_FAIL -I..
LDFLAGS      = -L /opt/local/lib -L../ 
SYSLIBS      = -ldb

TSDB_LIB     = ../libtsdb.a


TARGETS      = tsdb-get \
		tsdb-set \
		tsdb-create \
		tsdb-info

all: $(TARGETS)

%.o: %.c %.h
	${CC} ${CFLAGS} ${INCLUDE} -c $< -o $@

tsdb-%: tsdb_%.o $(TSDB_LIB) ../seatest.o
	$(CC) $(LDFLAGS) tsdb_$*.o $(TSDB_LIB) ../seatest.o  $(SYSLIBS) -o $@

test-%: test_%.o $(TEST_LIBS) ../seatest.o
	$(CC) $(LDFLAGS) test_$*.o ../seatest.o $(TEST_LIBS) $(SYSLIBS) -o $@

clean:
	rm -f ${TARGETS} *.o *~ test-* tsdb-*

.SECONDARY: $(TEST_LIBS)
