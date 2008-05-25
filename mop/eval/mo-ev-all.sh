#!/bin/bash

[ -n "$1" ] || { echo "Usage: mo-ev-all <tasks>" ; exit 1 ; }

while [ -n "$1" ] ; do
	for user in `bin/mo-get-users` ; do
		echo -e "\n### USER $user TASK $1 ###\n"
		bin/ev $user $1
	done
	shift
done