#!/bin/bash

CUR_LIST=`mktemp`
NEW_LIST=$1
XML=./VendorIds.xml

grep "id=" $XML | cut -d '"' -f 2 > $CUR_LIST
for existing_vendor in `cat $CUR_LIST`
do
	sed -i "\|$existing_vendor|d" $NEW_LIST
done
sed -i '/<\/VendorIds>/d' $XML
awk -F "\t" '{ print "\t<Vendor id=\"" $2 "\">\n\t\t<Name>" $1 "</Name>\n\t\t<Webpage></Webpage>\n\t</Vendor>"}' $NEW_LIST >> $XML
echo '</VendorIds>' >> $XML
