#!/bin/bash
# Create contestant directories with their solutions and test logs

[ -f config ] || { echo "Missing config file, check cwd." ; exit 1 ; }
set -e
. config

H=`pwd`
cd $MO_ROOT
rm -rf users2
mkdir users2
cd users2

for a in `cd $H && bin/mo-get-users` ; do
	echo "Creating $a"
	mkdir $a $a/$a
	chown root.$a $a
	chmod 750 $a
	cp -a `find $H/template -type f -name ".*"` $a/$a/
	if [ -d $H/solutions/$a ] ; then cp -a $H/solutions/$a $a/$a/solutions ; fi
	if [ -d $H/testing/$a ] ; then cp -a $H/testing/$a $a/$a/testing ; fi
	chown $a.$a $a/$a -R
	chmod 700 $a/$a
done