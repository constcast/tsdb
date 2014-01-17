#!/bin/bash

export PATH=$PATH:../bin

if [ ! -f test-tsdb.tsdb ];
then 
	tsdb-create test-tsdb.tsdb 60
fi

i=0
while [ $i -lt 200 ]; do
	(
		tsdb-set test-tsdb.tsdb test-$i 100
	)&
	let i=$i+1
done

echo "Started processes. Waiting for them to finish!"
wait
echo "Done"
