#!/usr/bin/python3
import argparse
import graphviz
import llvmlite.binding as llvm
from find_duplicated_links import find_duplicated_links


def find_terminating_blocks(basic_block):
    # verify there is no other terminating instruction than the last one
    opcodes = '\n'.join(
        [inst.opcode for inst in basic_block.instructions][:-1]
    )
    if 'ret' in opcodes or 'br' in opcodes:
        # print("="*20)
        # print(basic_block.function.name, ":")
        # print(opcodes)
        raise ValueError(
            'Block should terminate at first ret or br instruction.'
        )
    return []


def is_return_adress_store(instruction):
    if instruction.opcode == 'store':
        if (
            list(instruction.operands)[1].name == 'return_address'
            and list(instruction.operands)[0].is_constant
        ):
            return True
    return False


def is_pc_store(instruction):
    if instruction.opcode == 'store':
        if (
            list(instruction.operands)[1].name == 'PC'
            and list(instruction.operands)[0].is_constant
        ):
            return True
    return False


def get_const_pc_store(instruction):
    maybe_const = list(instruction.operands)[0]
    if maybe_const.is_constant:
        return maybe_const.get_constant_value()
    else:
        return 0


def main(args):
    graph = graphviz.Digraph(args.output)

    module = None

    with open(args.file, 'br') as ll_file:
        module = llvm.parse_bitcode(ll_file.read())
        module.verify()

    if module is not None:
        for func in module.functions:
            if func.name[:5] != 'Func_':
                continue
            if args.isolated:
                graph.node(func.name, fillcolor='#00ff004f', style='filled')

            for _, bb in enumerate(func.blocks):

                stored_pcs = list(
                    map(
                        get_const_pc_store,
                        filter(is_pc_store, bb.instructions),
                    )
                )
                stored_rets = list(
                    map(
                        get_const_pc_store,
                        filter(is_return_adress_store, bb.instructions),
                    )
                )

                for ret in stored_rets:
                    graph.node(
                        f'{func.name}', fillcolor='#00ff004f', style='filled'
                    )
                    graph.edge(f'{func.name}', f'Func_{ret:X}', color='blue')

                for pc in stored_pcs[:-1]:
                    graph.node(
                        f'{func.name}', fillcolor='#00ff004f', style='filled'
                    )
                    graph.edge(f'{func.name}', f'Func_{pc:X}')

                if stored_pcs:
                    graph.edge(
                        f'{func.name}', f'Func_{stored_pcs[-1]:X}', color='red'
                    )

    if args.check_duplicate:
        links = find_duplicated_links(graph.source)
        if links:
            print('WARNING: duplicated links')
        for a, b in links:
            print(f'\t{a} -> {b}')

    if not args.no_visual:
        graph.render(directory=args.output_dir)


if __name__ == '__main__':

    parser = argparse.ArgumentParser()
    parser.add_argument('file')
    parser.add_argument(
        '-i', '--isolated', help='Add isolated nodes', action='store_true'
    )
    parser.add_argument(
        '-d', '--output_dir', help='output directory', default='.'
    )
    parser.add_argument(
        '-o', '--output', help='output name', default='module_graph'
    )
    parser.add_argument('--check-duplicate', action='store_true')
    parser.add_argument(
        '-n', '--no-visual', help='do not output graph', action='store_true'
    )

    args = parser.parse_args()

    main(args)
