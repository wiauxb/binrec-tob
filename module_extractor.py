#!/usr/bin/python3
import llvmlite.binding as llvm
import re
import graphviz
import argparse

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
    graph = graphviz.Digraph('module_graph')

    module = None

    with open(args.file, "r") as ll_file:
        module = llvm.parse_assembly(ll_file.read())
        module.verify()

    if module != None:
        for func in module.functions:
            if func.name[:5] != "Func_":
                continue
            if args.isolated:
                graph.node(func.name)
            
            for i, bb in enumerate(func.blocks):

                stored_pcs = list(map(get_const_pc_store, filter(is_pc_store, bb.instructions)))

                if not stored_pcs:
                    continue

                for pc in stored_pcs[:-1]:
                    graph.edge(f"{func.name}", f"Func_{pc:X}")

                graph.edge(f"{func.name}", f"Func_{stored_pcs[-1]:X}", color="red")

                # if f"Func_{hex(int(last_stored_pc))}" not in list(module.functions):
                #     raise Exception(f"L'adresse {last_stored_pc:0X} n'est pas un successeur de {func.name}.")



            # blocks = list(func.blocks)
            # if len(blocks) == 0:
            #     # print(func.name, "as no blocks: ", func.is_declaration)
            #     continue
            # last_block = blocks[-1]
            # insts = list(last_block.instructions)
            # last_inst = insts[-1]
            # if re.search(r"!lastpc", str(last_inst)):
            #     concat_insts = "\n".join(str(i) for i in insts[-5:])
            #     PCs = re.findall(r'store i32 (\d*), i32\* @PC,', concat_insts)
            #     if len(PCs) > 0:
            #         supposed_return_addr = hex(int(PCs[-1]))[2:]
            #         # print(supposed_return_addr)
            #         supposed_following_func = f"Func_{supposed_return_addr}"
            #         if supposed_following_func in [f.name for f in module.functions]:
            #             graph.edge(func.name, supposed_following_func)
            #         else:
            #             graph.edge(func.name, f"SYMB_{supposed_return_addr}")
            #     # if len(PCs) > 1:
            #     #     pass
            #         # print("="*20)
            #         # print(func.name, ":")
            #         # print(concat_insts)


    graph.render(directory=args.output_dir)

if __name__ == "__main__":

    parser = argparse.ArgumentParser()
    parser.add_argument("file")
    parser.add_argument("-i", "--isolated", help="Add isolated nodes", action="store_true")
    parser.add_argument('-d', '--output_dir', help="output directory", default='.')

    args = parser.parse_args()

    main(args)