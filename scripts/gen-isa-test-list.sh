#!/usr/bin/env bash

function parse-tests()
{
    > contributors
    while read -r line; do
	item=$(echo "$line" | awk -F "(" '{print $2}' | awk -F ")" '{print $1}')
        echo "$item"
    done <<< $(grep "ADD_INSN_TEST" tests/isa-test.c )
}

parse-tests
