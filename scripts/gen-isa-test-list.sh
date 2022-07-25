#!/usr/bin/env bash

function parse-tests()
{
    # TODO: cache the parsed test items
    while read -r line; do
	echo "$line" | awk -F "(" '{print $2}' | awk -F ")" '{print $1}'
    done < <(grep "ADD_INSN_TEST" tests/isa-test.c)
}

parse-tests
