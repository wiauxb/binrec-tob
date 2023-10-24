#!/usr/bin/python3
import llvmlite.binding as llvm
import re
import graphviz
import argparse
from find_duplicated_links import find_duplicated_links

def find_terminating_blocks(basic_block):
    # verify there is no other terminating instruction than the last one
    opcodes = "\n".join([inst.opcode for inst in basic_block.instructions][:-1])
    if "ret" in opcodes or "br" in opcodes:
        # print("="*20)
        # print(basic_block.function.name, ":")
        # print(opcodes)
        raise Exception("Block should terminate at first ret or br instruction.")
    return []

def is_pc_store(instruction):
    if instruction.opcode == "store":
        if list(instruction.operands)[1].name == "PC" and list(instruction.operands)[0].is_constant:
           return True
    return False

def get_const_pc_store(instruction):
    maybe_const = list(instruction.operands)[0]
    if maybe_const.is_constant:
        return maybe_const.get_constant_value()
    else:
        return 0

def get_block_succs(basic_block):
    return []

def find_successor(successors, pc):
    return None

def main(args):

    module = None

    with open(args.file, "br") as ll_file:
        module = llvm.parse_bitcode(ll_file.read())
        module.verify()

    if module != None:
        for f in module.functions:
            for bb in f.blocks:
                for inst in bb.instructions:
                    for att in inst.attributes:
                        print(att)

if __name__ == "__main__":

    parser = argparse.ArgumentParser()
    parser.add_argument("file")
    parser.add_argument("-i", "--isolated", help="Add isolated nodes", action="store_true")
    parser.add_argument('-d', '--output_dir', help="output directory", default='.')
    parser.add_argument('-o', '--output', help="output name", default='module_graph')
    parser.add_argument("--check-duplicate", action="store_true")
    parser.add_argument("-n", "--no-visual", help="do not output graph", action="store_true")

    args = parser.parse_args()

    main(args)