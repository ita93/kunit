#!/bin/bash 
if [ "$(uname -m)" != "x86_64" ]; then
    	echo "This system is NOT x86_64, need cross compile"
	./tools/testing/kunit/kunit.py run --kunitconfig=mm/.kunitconfig --arch=x86_64 --kernel_args=movablecore=256M --cross_compile=x86_64-linux-gnu-
else
	echo "This system is x86_64"
	./tools/testing/kunit/kunit.py run --kunitconfig=mm/.kunitconfig --arch=x86_64 --kernel_args=movablecore=256M
fi
