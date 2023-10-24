#!/usr/bin/python3
import re

def main(args):
    with open(args.file, "r") as f:
        graph = f.read()
    for a, b in find_duplicated_links(graph):
        print(f"{a} -> {b}")

def find_duplicated_links(graph):
    result = []
    for arrow in re.finditer(r'\s*(Func_[a-fA-F\d]{7,8}) -> (Func_[a-fA-F\d]{7,8}).*\n\s*\1 -> \2', graph, re.MULTILINE):
        if arrow[1] != arrow[2]:
            result.append((arrow[1], arrow[2]))
    return result

if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("file", help="filepath of the graph")

    args = parser.parse_args()
    main(args)