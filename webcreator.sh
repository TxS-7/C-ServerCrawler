#!/bin/bash

if [ $# -ne 4 ]
then
	echo "Usage: $0 root_directory text_file w p"
	exit 1
fi


root_directory=$1
# Check if root_directory exists and is readable
if [ ! -d $root_directory ] || [ ! -r $root_directory ] || [ ! -x $root_directory ] || [ ! -w $root_directory ]
then
	echo "Can't access root directory: $root_directory"
	exit 2
fi


text_file=$2
# Check if text_file exists and is readable
if [ ! -f $text_file ] || [ ! -r $text_file ] || [ `cat $text_file | wc -l` -lt 10000 ]
then
	echo "Invalid text file: $text_file"
	exit 3
fi


# Check if w and p are positive integers
if [[ ! "$3" =~ ^[0-9][0-9]*$ ]] || [[ ! "$4" =~ ^[0-9][0-9]*$ ]] || [ "$3" -eq 0 ] || [ "$4" -eq 0 ]
then
	echo "w and p have to be positive integers"
	exit 4
fi


# Check if root_directory is not empty
# If it is not, remove everything inside it
if [ `ls -A "$root_directory" | wc -l` -ne 0 ]
then
	echo "# Warning: Directory not empty, purging ..."
	rm -rf "${root_directory}"/*
fi



w=$3
p=$4
let f="($p / 2) + 1"
q=0
if [ "$w" -gt 1 ]
then
	if [ "$w" -eq 2 ] && [ "$p" -eq 1 ]
	then
		q=1
	else
		let q="($w / 2) + 1"
	fi
fi
tf_lines=$(cat ${text_file} | wc -l)


# Create the list of web page names for each website
# and a list of number of incoming links for each web page
# to find pages without incoming links
declare -a webpages
declare -a link_count

i=0
while [ "$i" -lt "$w" ]
do
	webpages[$i]="$(shuf -i 1-99999 -n $p)"

	# Each array position represents a website and contains a string of counters
	j=0
	link_count[$i]="0"
	while [ "$j" -lt `expr $p - 1` ]
	do
		link_count[$i]="${link_count[$i]} 0"
		j=`expr $j + 1`
	done

	i=`expr $i + 1`
done

# Function used to change web page incoming link counters
# (https://stackoverflow.com/questions/16487258/how-to-declare-2d-array-in-bash)
function aset {
	IFS=' ' read -r -a tmp <<< "${link_count[$1]}"
	tmp[$2]=$3
	link_count[$1]="${tmp[@]}"
}



# Create the websites
i=0
while [ "$i" -lt "$w" ]
do
	echo "# Creating web site $i ..."
	mkdir "${root_directory}/site$i"

	j=0
	while [ "$j" -lt "$p" ]
	do
		line=`expr $j + 1`
		# Get the unique random number of the page's name
		number=$(echo "${webpages[$i]}" | head -n $line | tail -n 1)
		filename="${root_directory}/site${i}/page${i}_${number}.html"

		# Calculate web page creation variables
		max=`expr $tf_lines - 1999` # Limit for k
		k=$(shuf -i 2-"$max" -n 1) # 1 < k < #lines - 2000
		m=$(shuf -i 1001-1999 -n 1) # 1000 < m < 2000



		# Create the random internal links
		internal_indexes=""
		# Special case: accept link to self
		if [ "$f" -eq "$p" ]
		then
			internal_indexes=$(shuf -i 0-`expr $p - 1` -n "$f")
		else
			internal_indexes=$(shuf -i 0-`expr $p - 1` -n `expr $f + 1` | sed "/^${j}$/d")
		fi

		# Create full links
		full_links=""
		first_line=1
		while read -r line
		do
			# Increase incoming link counter
			# link_count[i][line] = 1
			aset $i $line 1

			# Get the page number at the given index
			number=$(echo "${webpages[$i]}" | head -n `expr $line + 1` | tail -n 1)

			if [ "$first_line" -eq 1 ]
			then
				full_links="page${i}_${number}.html"
				first_line=0
			else
				full_links=$(echo -e "${full_links}\npage${i}_${number}.html")
			fi
		done <<< "$internal_indexes"


		# Create the random external links
		external_indexes=""
		if [ "$w" -gt 1 ] # At least one other website
		then
			if [ "$w" -eq 2 ] && [ "$p" -eq 1 ]
			then
				# Only one external link can exist
				external_indexes="0"
			else
				# p webpages for every website except the current one
				let limit="($w - 1) * $p"
				external_indexes=$(shuf -i 0-`expr $limit - 1` -n "$q")
			fi
		fi

		# Create full links
		full_links_ext=""
		if [ ! -z "$external_indexes" ]
		then
			first_line=1
			while read -r line
			do
				# Get the website in which the webpage belongs
				let site_index="$line / $p"
				# If we are after the current website, increase index by 1 to ignore it
				if [ "$site_index" -ge "$i" ]
				then
					site_index=`expr $site_index + 1`
				fi

				# Get the page number
				page_index=`expr $line % $p`
				number=$(echo "${webpages[$site_index]}" | head -n `expr $page_index + 1` | tail -n 1)

				# Increase incoming link counter
				# link_count[site_index][page_index] = 1
				aset $site_index $page_index 1

				if [ "$first_line" -eq 1 ]
				then
					full_links_ext="/site${site_index}/page${site_index}_${number}.html"
					first_line=0
				else
					full_links_ext=$(echo -e "${full_links_ext}\n/site${site_index}/page${site_index}_${number}.html")
				fi
			done <<< "$external_indexes"
		fi


		# Combine links
		if [ ! -z "$full_links_ext" ]
		then
			full_links=$(echo -e "${full_links}\n${full_links_ext}")
		fi



		echo "#   Creating page $filename with $m lines starting at line $k ..."
		# Write initial HTML headers
		echo '<!DOCTYPE html>' > "$filename"
		echo '<html>' >> "$filename"
		echo '   <body>' >> "$filename"


		divisor=`expr $f + $q`
		# Calculate ceiling of m / (f + q)
		# https://stackoverflow.com/questions/2394988/get-ceiling-integer-from-number-in-linux-bash
		let lines_to_copy="(m + $divisor - 1) / $divisor"

		remaining_lines=$m
		curr_line=$k
		remaining_links=`expr $f + $q`
		# Get internal and external links in random order
		randomized_links=$(echo "$full_links" | shuf -n $remaining_links)
		curr_link=0

		while [ "$remaining_lines" -gt 0 ]
		do
			if [ "$remaining_lines" -lt "$lines_to_copy" ]
			then
				lines_to_copy=$remaining_lines
			fi

			# Copy text_file lines to the web page
			text=$(cat "$text_file" | head -n `expr $curr_line + $lines_to_copy - 1` | tail -n $lines_to_copy)
			while read -r line
			do
				# Remove '\r' if it exists and print line to the file
				echo "   $line<br>" | tr -d '\r' >> $filename
			done <<< "$text"

			# Add a link after the text segment
			link=$(echo "$randomized_links" | head -n `expr $curr_link + 1` | tail -n 1)

			if [[ $link =~ ^/ ]]
			then
				# Link to external webpage in the form: /site<i>/page<i>_...
				echo "#   Adding link to ${root_directory}${link}"
			else
				# Link to internal webpage in the form: page<i>_...
				echo "#   Adding link to ${root_directory}/site${i}/${link}"
			fi

			echo "   <a href=\"${link}\">link_${curr_link}</a><br>" >> $filename
			remaining_links=`expr $remaining_links - 1`
			curr_link=`expr $curr_link + 1`

			remaining_lines=`expr $remaining_lines - $lines_to_copy`
			curr_line=`expr $curr_line + $lines_to_copy`
		done


		# Write final HTML headers
		echo '   </body>' >> "$filename"
		echo '</html>' >> "$filename"

		j=`expr $j + 1`
	done

	i=`expr $i + 1`
done


# Check if there are pages without incoming links
i=0
no_incoming=0
while [ "$i" -lt "$w" ]
do
	line="${link_count[$i]}"

	j=0
	while [ "$j" -lt "$p" ]
	do
		count=$(echo "$line" | cut -d " " -f `expr $j + 1`)
		if [ "$count" -eq 0 ]
		then
			no_incoming=1
			break
		fi
		j=`expr $j + 1`
	done

	i=`expr $i + 1`
done

if [ "$no_incoming" -eq 0 ]
then
	echo "# All pages have at least one incoming link"
else
	echo "# Not all pages have at least one incoming link"
fi

echo "# Done."
