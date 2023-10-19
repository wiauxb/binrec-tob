import errno
import json
import logging
import os
import re
import shutil
import subprocess
import textwrap
from pathlib import Path
from typing import List, Tuple, Union

from binrec.campaign import (
    Campaign,
    TraceArg,
    TraceArgType,
    TraceInputFile,
    TraceParams,
    patch_s2e_project,
)

from .env import (
    INPUT_FILES_DIRNAME,
    campaign_filename,
    get_trace_dirs,
    input_files_dir,
    merged_trace_dir,
    project_dir,
    s2e_config_filename,
)
from .errors import BinRecError

logger = logging.getLogger("binrec.project")


def listing() -> List[str]:
    try:
        output = subprocess.check_output(["s2e", "info"])
    except subprocess.CalledProcessError:
        raise BinRecError("s2e run failed to get project list")

    d = json.loads(output)
    return d["projects"].keys()


def _list() -> None:
    for proj in listing():
        print(proj)


def add_campaign_trace(
    project: str, args: List[str], symbolic_indexes: List[int] = None, name: str = None
) -> TraceParams:
    """
    Add a new trace to an existing campaign.

    :param project: the project name
    :param args: the full command line arguments, including concrete and symbolic
    :param symbolic_indexes: the list of argument indexes in ``args`` that are symbolic
    :param name: the new trace name
    """
    trace_args = TraceParams.create_trace_args(args, symbolic_indexes or [])
    params = TraceParams(trace_args, name=name)

    campaign = Campaign.load_project(project, resolve_input_files=False)
    logger.info(
        "adding new trace to campaign %s: %s (symbolic args: %s)",
        project,
        params.command_line_args,
        params.symbolic_indexes,
    )
    campaign.traces.append(params)

    campaign.save()

    return params


def _resolve_trace_name_or_id(
    campaign: Campaign, name_or_id: Union[str, int]
) -> Tuple[int, TraceParams]:
    """
    Resolve a trace name or id to a trace. If ``name_or_id`` is a string
    representing a number then it is treated as the trace id.

    :returns: a tuple of ``(trace_id, trace)``
    :raises KeyError: the trace does not exist within the campaign
    """
    try:
        trace_id = int(name_or_id)
        if trace_id < 0:
            trace_id = len(campaign.traces) + trace_id

        if trace_id >= 0 and trace_id < len(campaign.traces):
            return trace_id, campaign.traces[trace_id]
    except ValueError:
        pass

    for trace_id, trace in enumerate(campaign.traces):
        if trace.name and trace.name == name_or_id:
            return trace_id, trace

    raise KeyError(f"trace does not exist: {name_or_id}")


def remove_campaign_trace(project: str, name_or_id: Union[str, int]) -> None:
    """
    Remove a trace from an existing campaign.

    :param project: project name
    :param name_or_id: trace name or id (see :meth:`Campaign.remove_trace`
    """
    campaign = Campaign.load_project(project, resolve_input_files=False)

    trace_id, trace = _resolve_trace_name_or_id(campaign, name_or_id)

    logger.info("removing trace %s/%s", project, trace.name or trace_id)
    campaign.remove_trace(trace_id)
    campaign.save()


def remove_campaign_all_traces(project: str) -> None:
    """
    Remove all traces from an existing campaign.
    """
    campaign = Campaign.load_project(project, resolve_input_files=False)

    logger.info("removing all traces from campaign: %s", project)
    campaign.traces = []
    campaign.save()


def set_trace_stdin(
    project: str, trace_name_or_id: Union[str, int], stdin: str
) -> None:
    """
    Set the stdin content for a single trace.

    :param project: project name
    :param trace_name_or_id: the existing trace name or id
    :param stdin: stdin content
    """
    campaign = Campaign.load_project(project, resolve_input_files=False)
    trace_id, trace = _resolve_trace_name_or_id(campaign, trace_name_or_id)
    trace.stdin = stdin
    logger.info("setting stdin content for %s/%s", project, trace.name or trace_id)
    campaign.save()


def add_trace_input_file(
    project: str,
    trace_name_or_id: Union[str, int],
    source: Path,
    destination: Path = None,
    permissions: Union[str, bool] = None,
) -> None:
    """
    Add a new trace input file to an existing trace.

    :param project: project name
    :param trace_name_or_id: the existing trace name or id
    :param source: source input file on the host filesystem
    """
    campaign = Campaign.load_project(project, resolve_input_files=False)
    trace_id, trace = _resolve_trace_name_or_id(campaign, trace_name_or_id)

    with open(source, "r") as _:
        # Verify that the file exists and we can read it
        pass

    if permissions in (None, ""):
        chmod = True  # default behavior: copy source file permissions
    else:
        chmod = permissions  # type: ignore

    input_file = TraceInputFile(source.absolute(), destination, chmod)
    trace.input_files.append(input_file)
    logger.info(
        "adding input file to %s/%s: %s -> %s",
        project,
        trace.name or trace_id,
        input_file.source,
        destination or f"./input_files/{input_file.source.name}",
    )
    campaign.save()


def remove_trace_input_file(
    project: str, trace_name_or_id: Union[int, str], filename: Path
) -> None:
    """
    Remove an input file from a trace. The ``filename`` parameter can either be the
    full path to remove or just the file basename.

    :param project: project name
    :param trace_name_or_id: trace name or id
    :param filename: filename or path to remove
    :raises KeyError: the input file does not exist
    """
    basename = filename.name if "/" not in str(filename) else None
    campaign = Campaign.load_project(project, resolve_input_files=False)
    trace_id, trace = _resolve_trace_name_or_id(campaign, trace_name_or_id)

    for input_file in trace.input_files:
        if filename == input_file.source or (
            basename and basename == input_file.source.name
        ):
            found = input_file
            break
    else:
        raise KeyError(f"input files does not eixst: {filename}")

    logger.info(
        "removing input file from %s/%s: %s",
        project,
        trace.name or trace_id,
        found.source,
    )
    trace.input_files.remove(found)
    campaign.save()


def _link_lifted_input_files(project_name: str) -> None:
    """
    Link the project's input files directory to the final lifted directory so that
    comarpison between the original and the lifted binary can be performed.
    """
    source = input_files_dir(project_name)
    dest = merged_trace_dir(project_name) / INPUT_FILES_DIRNAME

    if source.is_dir() and not dest.is_dir():
        dest.symlink_to(source)


def _run_trace_setup(campaign: Campaign, trace: TraceParams, cwd: Path) -> None:
    """
    Run the trace setup actions for a given campaign and trace. If the trace does not
    contain any setup actions, the campaign setup actions will be run.

    :param campaign: the campaign
    :param trace: the trace
    :param cwd: the current working directory to execute the actions from
    """
    setup = trace.setup or campaign.setup
    if not setup:
        return

    logger.info("running setup actions")
    script = "\n".join(setup)
    subprocess.run(["/bin/bash", "--noprofile"], input=script.encode(), cwd=str(cwd))
    logger.info("setup actions completed")


def _run_trace_teardown(campaign: Campaign, trace: TraceParams, cwd: Path) -> None:
    """
    Run the trace teardown actions for a given campaign and trace. If the trace does not
    contain any teardown actions, the campaign teardown actions will be run.

    :param campaign: the campaign
    :param trace: the trace
    :param cwd: the current working directory to execute the actions from
    """
    teardown = trace.teardown or campaign.teardown
    if not teardown:
        return

    logger.info("running teardown actions")
    script = "\n".join(teardown)
    subprocess.run(["/bin/bash", "--noprofile"], input=script.encode(), cwd=str(cwd))
    logger.info("teardown actions completed")


def run_campaign(project_or_campaign: Union[str, Campaign]) -> None:
    """
    Run an entire campaign and all traces.

    :param project_or_campaign: the project name (``str``) or the campaign object to run
    """
    if isinstance(project_or_campaign, str):
        campaign = Campaign.load_project(project_or_campaign)
    elif isinstance(project_or_campaign, Campaign):
        campaign = project_or_campaign
    else:
        raise TypeError("expected project name (str) or campaign object")

    for trace in campaign.traces:
        _run_campaign_trace(campaign, trace)


def run_campaign_trace(project: str, trace_name_or_id: Union[int, str]) -> None:
    """
    Run a single trace within a campaign.

    :param project: the project name
    :param trace_name_or_id: the trace name or id to run (see
        :meth:`Campaign.get_trace`)
    """
    campaign = Campaign.load_project(project)
    _, trace = _resolve_trace_name_or_id(campaign, trace_name_or_id)
    _run_campaign_trace(campaign, trace)


def _run_campaign_trace(campaign: Campaign, trace: TraceParams) -> None:
    """
    Internal method to run a single trace within a campaign.

    :param campaign: the campaign
    :param trace: the trace
    """
    trace.setup_input_file_directory(campaign.project)
    trace.write_config_script(campaign.project)

    logfile = _get_next_trace_log_filename(campaign.project)
    logger.info(
        "running campaign trace: %s/%s (saving S2E log to: %s)",
        campaign.project,
        trace.name or "<anonymous trace>",
        logfile,
    )
    try:
        subprocess.check_call(
            ["s2e", "run", "--no-tui", campaign.project],
            stdout=logfile.open("w"),
            stderr=subprocess.STDOUT,
        )
    except subprocess.CalledProcessError:
        raise BinRecError(
            f"s2e run failed for project: {campaign.project}, for more information "
            f"view the log file at {logfile}"
        )


def _get_next_trace_log_filename(project: str) -> Path:
    """
    Get the next log file name prior to running a trace.
    """
    trace_nums = []
    for entry in get_trace_dirs(project):
        try:
            number = int(entry.name.split("-")[-1])
            trace_nums.append(number)
        except ValueError:
            pass

    i = 0
    while i in trace_nums:
        i += 1

    return project_dir(project) / f"s2e-out-{i}.log"


def validate_campaign(project_or_campaign: Union[str, Campaign]) -> None:
    """
    Validate the lift results for an entire campaign.

    :param project_or_campaign: the project name or the campaign object to validate
    """
    if isinstance(project_or_campaign, str):
        campaign = Campaign.load_project(project_or_campaign)
    elif isinstance(project_or_campaign, Campaign):
        campaign = project_or_campaign
    else:
        raise TypeError("expected project name (str) or campaign object")

    for trace in campaign.traces:
        _validate_campaign_trace(campaign, trace)


def validate_campaign_trace(project: str, trace_name_or_id: Union[int, str]) -> None:
    """
    Validate the lift result of a single trace within a campaign.

    :param project: project name
    :param  trace_name_or_id: the trace name or id to validate (see
        :meth:`Campaign.get_trace`)
    """
    campaign = Campaign.load_project(project)
    _, trace = _resolve_trace_name_or_id(campaign, trace_name_or_id)
    _validate_campaign_trace(campaign, trace)


def validate_campaign_with_args(project: str, args: List[str]) -> None:
    """
    Validate the list result against the provided command line arguments.

    :param args: the command line arguments to validate with
    """
    campaign = Campaign.load_project(project)
    trace = TraceParams(args=[TraceArg(TraceArgType.concrete, arg) for arg in args])
    _validate_campaign_trace(campaign, trace)


def _validate_campaign_trace(campaign: Campaign, trace: TraceParams) -> None:
    """
    Compare the original binary against the lifted binary for a given sample of
    command line arguments. This method runs the original and the lifted binary
    and then compares the process return code, stdout, and stderr content. An
    ``AssertionError`` is raised if any of the comparison criteria does not match
    between the original and lifted sample.

    :param campaign: the campaign
    :param trace: the trace
    """
    project = campaign.project
    logger.info(
        "Validating project %s with arguments: %s", project, trace.command_line_args
    )

    merged_dir = merged_trace_dir(project)
    lifted = str(merged_dir / "recovered")
    original = str(merged_dir / "binary")
    target_path = merged_dir / "test-target"
    target = str(target_path)

    if target_path.is_symlink():
        target_path.unlink()

    # We link to the binary we are running to make sure argv[0] is the same
    # for the original and the lifted program.
    os.link(original, target)

    trace.setup_input_file_directory(project)
    _link_lifted_input_files(project)

    stdin_file = subprocess.PIPE if trace.stdin else subprocess.DEVNULL

    _run_trace_setup(campaign, trace, merged_dir)
    logger.debug(">> running original sample with args: %s", trace.command_line_args)
    original_proc = subprocess.Popen(
        [target] + trace.command_line_args,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        stdin=stdin_file,
        cwd=str(merged_dir),
    )

    if trace.stdin and original_proc.stdin:
        original_proc.stdin.write(trace.stdin.encode())
        original_proc.stdin.close()

    original_proc.wait()
    os.remove(target)

    _run_trace_teardown(campaign, trace, merged_dir)

    original_stdout = original_proc.stdout.read()  # type: ignore
    original_stderr = original_proc.stderr.read()  # type: ignore

    os.link(lifted, target)
    _run_trace_setup(campaign, trace, merged_dir)

    logger.debug(">> running recovered sample with args: %s", trace.command_line_args)

    lifted_proc = subprocess.Popen(
        [target] + trace.command_line_args,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        stdin=stdin_file,
        cwd=str(merged_dir),
    )

    if trace.stdin and lifted_proc.stdin:
        lifted_proc.stdin.write(trace.stdin.encode())
        lifted_proc.stdin.close()

    # Only close if stdin is a file / pipe (unsupported at this time)
    lifted_proc.wait()
    os.remove(target)

    _run_trace_teardown(campaign, trace, merged_dir)

    lifted_stdout = lifted_proc.stdout.read()  # type: ignore
    lifted_stderr = lifted_proc.stderr.read()  # type: ignore

    assert (
        original_proc.returncode == lifted_proc.returncode
    ), "recovered exit code does not match original"

    if trace.match_stdout is True:
        assert (
            original_stdout == lifted_stdout
        ), "recovered stdout content does not match original"
    elif isinstance(trace.match_stdout, str):
        assert (
            re.match(trace.match_stdout, lifted_stdout.decode(errors="replace"))
            is not None
        ), "regex pattern for stdout content does not match"

    if trace.match_stderr is True:
        assert (
            original_stderr == lifted_stderr
        ), "recovered stderr content does not match original"
    elif isinstance(trace.match_stderr, str):
        assert (
            re.match(trace.match_stderr, lifted_stderr.decode(errors="replace"))
            is not None
        ), "regex pattern for stderr content does not match"

    logger.info(
        "Output from %s's original and lifted binaries match for args: %s",
        project,
        str(trace.command_line_args),
    )


def new_project(
    project_name: str, binary_filename: Path, template: Union[Path, Campaign] = None
) -> Path:
    """
    Create a new S2E analysis project.

    :param project_name: the analysis project name
    :param binary_filename: the path to binary being analyzed
    :param template: a path to an existing campaign JSON file to use as the basis for
        the new project
    :returns: the path to the project directory
    """
    project_path = project_dir(project_name)
    if project_path.is_dir():
        raise FileExistsError(
            errno.EEXIST, os.strerror(errno.EEXIST), str(project_path)
        )

    if isinstance(template, Path):
        campaign = Campaign.load_json(
            binary_filename, template, project_name, resolve_input_files=True
        )
    elif isinstance(template, Campaign):
        campaign = template
    else:
        campaign = Campaign(binary_filename, project=project_name)

    logger.info("Creating project: %s", project_name)
    callargs = ["s2e", "new_project", "--name", project_name, str(binary_filename)]

    try:
        subprocess.check_call(callargs)
    except subprocess.CalledProcessError:
        raise BinRecError(f"s2e run failed for project: {project_name}")

    # create the input files directory
    input_files = input_files_dir(project_name)
    input_files.mkdir()

    # link to the sample binary so we can easily reference it later on
    binary = project_path / "binary"
    binary.symlink_to(binary_filename.absolute())

    # Update the configuration file to load our plugins and map in the input files
    # directory to the analysis VM
    with open(s2e_config_filename(project_name), "a") as file:
        file.write(
            f"""
add_plugin(\"ELFSelector\")
add_plugin(\"FunctionMonitor\")
add_plugin(\"FunctionLog\")
pluginsConfig.FunctionLog = {{
    baseDirs = {{
        "{project_path}"
    }},
    saveInterval = 1000 -- export every 1000 basic blocks
}}
add_plugin(\"ExportELF\")
pluginsConfig.ExportELF = {{
    baseDirs = {{
        "{project_path}"
    }},
    exportInterval = 1000 -- export every 1000 basic blocks
}}

table.insert(pluginsConfig.HostFiles.baseDirs, "{input_files}")
"""
        )

    patch_s2e_project(project_name)
    campaign.save()

    return project_path


def describe_campaign(project_name: str) -> None:
    """
    Describe a campaign by printing all information to stdout.

    :param project_name: the project name
    """
    campaign = Campaign.load_project(project_name)
    print(campaign.project)
    print("=" * len(campaign.project))
    print("Campaign File:", campaign_filename(project_name))
    print("Sample Binary:", campaign.binary)
    if campaign.setup:
        print(f"Global Setup ({len(campaign.setup)}):")
        for line in campaign.setup:
            print(" ", line)
        print()

    if campaign.teardown:
        print(f"Global Teardown ({len(campaign.teardown)}):")
        for line in campaign.teardown:
            print(" ", line)
        print()

    print(f"Traces ({len(campaign.traces)}):")
    for index, trace in enumerate(campaign.traces):
        name = trace.name or "(anonymous trace)"
        print(" ", name)
        print(" ", "-" * len(name))
        print("  Id:", index)
        print("  Command Line Arguments:", trace.command_line_args)
        print("  Symbolic Indexes:", ", ".join(str(i) for i in trace.symbolic_indexes))

        if trace.input_files:
            print(f"  Input Files ({len(trace.input_files)}):")
            for input_file in trace.input_files:
                print("   ", input_file.source)
                if input_file.destination:
                    print("     ", "Destination:", input_file.destination)
                if isinstance(input_file.permissions, bool):
                    if input_file.permissions:
                        print("      [Preserve source permissions]")
                    else:
                        print("      [Use default permissions]")
                else:
                    print(f"      [chmod {input_file.permissions}]")

        if trace.setup:
            print(f"  Setup ({len(trace.setup)}):")
            for line in trace.setup:
                print("   ", line)
            print()
        elif campaign.setup:
            print("  [Inherit global setup]")

        if trace.teardown:
            print(f"  Teardown ({len(trace.teardown)}):")
            for line in trace.teardown:
                print("   ", line)
            print()
        elif campaign.teardown:
            print("  [Inherit global teardown]")

        if trace.stdin:
            print("  stdin:")
            print(textwrap.indent(trace.stdin, "    "))

        print()


def clear_project_trace_data(project: str) -> None:
    """
    Delete all trace directories from the project.
    """
    logger.info("clearing trace directory for project: %s", project)
    for dirname in get_trace_dirs(project):
        logger.debug("deleting trace directory: %s", dirname)
        shutil.rmtree(dirname)

    merged = merged_trace_dir(project)
    if merged.is_dir():
        logger.debug("deleting merged trace directory: %s", merged)
        shutil.rmtree(merged)


def add_trace_setup(
    project: str, trace_name_or_id: Union[str, int], command: str
) -> None:
    """
    Add a new setup command to an existing trace.

    :param project: project name
    :param trace_name_or_id: trace name or id
    :param command: bash command to execute during trace setup
    """
    campaign = Campaign.load_project(project)
    _, trace = _resolve_trace_name_or_id(campaign, trace_name_or_id)
    logger.info(
        "adding new setup command to %s/%s: %s", project, trace_name_or_id, command
    )
    trace.setup.append(command)
    campaign.save()


def add_trace_teardown(
    project: str, trace_name_or_id: Union[str, int], command: str
) -> None:
    """
    Add a new teardown command to an existing trace.

    :param project: project name
    :param trace_name_or_id: trace name or id
    :param command: bash command to execute during trace teardown
    """
    campaign = Campaign.load_project(project)
    _, trace = _resolve_trace_name_or_id(campaign, trace_name_or_id)
    logger.info(
        "adding new teardown command to %s/%s: %s", project, trace_name_or_id, command
    )
    trace.teardown.append(command)
    campaign.save()


def main() -> None:
    import argparse

    from .core import enable_binrec_debug_mode, init_binrec

    init_binrec()

    parser = argparse.ArgumentParser()
    parser.add_argument(
        "-v", "--verbose", action="count", help="enable verbose logging"
    )

    subparsers = parser.add_subparsers(dest="current_parser")

    new_proj = subparsers.add_parser("new")
    new_proj.add_argument("project", help="Name of new analysis project")
    new_proj.add_argument("binary", type=Path, help="Path to binary used in analysis")
    new_proj.add_argument("template", default="", help="create campaign from template")

    subparsers.add_parser("list-projects")

    add_trace = subparsers.add_parser("add-trace")
    add_trace.add_argument("project", help="Project name")
    add_trace.add_argument("--name", action="store", help="trace name")
    add_trace.add_argument(
        "-s",
        "--symbolic-indexes",
        action="store",
        help='symbolic argument indexes in the form of "ARG_1 ARG_2 ... ARG_N"',
    )
    add_trace.add_argument("args", nargs="*", help="command line arguments")

    remove_trace = subparsers.add_parser("remove-trace")
    remove_trace.add_argument("project", help="Project name")
    remove_trace.add_argument(
        "-i", "--id", action="store_true", help="force treating 'name' as the trace id"
    )
    remove_trace.add_argument("--all", action="store_true", help="remove all traces")
    remove_trace.add_argument(
        "name", nargs="?", help="trace name (or trace id if --id is provided)"
    )

    run = subparsers.add_parser("run")
    run.add_argument("project", help="project name")

    run_trace = subparsers.add_parser("run-trace")
    run_trace.add_argument(
        "-i", "--id", action="store_true", help="force treating 'name' as the trace id"
    )
    run_trace.add_argument(
        "--last", action="store_true", help="run the last registered trace"
    )
    run_trace.add_argument("project", help="Project name")
    run_trace.add_argument(
        "name", nargs="?", help="trace name (or trace id if --id is provided)"
    )

    validate = subparsers.add_parser("validate")
    validate.add_argument("project", help="Project name")

    validate_args = subparsers.add_parser("validate-args")
    validate_args.add_argument("project", help="Project name")
    validate_args.add_argument(
        "args", nargs="*", help="command line arguments to validate against"
    )

    validate_trace = subparsers.add_parser("validate-trace")
    validate_trace.add_argument(
        "-i", "--id", action="store_true", help="force treating 'name' as the trace id"
    )
    validate_trace.add_argument("project", help="Project name")
    validate_trace.add_argument(
        "name", help="trace name (or trace id if --id is provided)"
    )

    describe = subparsers.add_parser("describe")
    describe.add_argument("project", help="Project name")

    clear_trace_data = subparsers.add_parser("clear-trace-data")
    clear_trace_data.add_argument("project", help="Project name")

    set_stdin = subparsers.add_parser("set-trace-stdin")
    set_stdin.add_argument("project", help="Project name")
    set_stdin.add_argument(
        "-i", "--id", action="store_true", help="force treating 'name' as the trace id"
    )
    set_stdin.add_argument("name", help="trace name (or trace id if --id is provided)")
    set_stdin.add_argument("stdin", help="stdin content")

    add_input_file = subparsers.add_parser("add-trace-input-file")
    add_input_file.add_argument("project", help="Project name")
    add_input_file.add_argument(
        "-i", "--id", action="store_true", help="force treating 'name' as the trace id"
    )
    add_input_file.add_argument(
        "name", help="trace name (or trace id if --id is provided)"
    )
    add_input_file.add_argument("source", help="source filename", type=Path)
    add_input_file.add_argument("destination", help="destination file path", nargs="?")
    add_input_file.add_argument(
        "permissions",
        help="destination file permissions, in cmod octal syntax",
        nargs="?",
    )

    remove_input_file = subparsers.add_parser("remove-trace-input-file")
    remove_input_file.add_argument("project", help="Project name")
    remove_input_file.add_argument(
        "-i", "--id", action="store_true", help="force treating 'name' as the trace id"
    )
    remove_input_file.add_argument(
        "name", help="trace name (or trace id if --id is provided)"
    )
    remove_input_file.add_argument("source", help="source filename or path", type=Path)

    add_setup = subparsers.add_parser("add-trace-setup")
    add_setup.add_argument("project", help="Project name")
    add_setup.add_argument(
        "-i", "--id", action="store_true", help="force treating 'name' as the trace id"
    )
    add_setup.add_argument("name", help="trace name (or trace id if --id is provided)")
    add_setup.add_argument("command", help="bash command to execute")

    add_teardown = subparsers.add_parser("add-trace-teardown")
    add_teardown.add_argument("project", help="Project name")
    add_teardown.add_argument(
        "-i", "--id", action="store_true", help="force treating 'name' as the trace id"
    )
    add_teardown.add_argument(
        "name", help="trace name (or trace id if --id is provided)"
    )
    add_teardown.add_argument("command", help="bash command to execute")

    args = parser.parse_args()

    if args.verbose:
        enable_binrec_debug_mode()

    if args.current_parser == "new":
        template = Path(args.template) if args.template else None
        new_project(args.project, args.binary, template)
    elif args.current_parser == "list-projects":
        _list()
    elif args.current_parser == "add-trace":
        if args.symbolic_indexes:
            symbolic_indexes = [int(i) for i in args.symbolic_indexes.split()]
        else:
            symbolic_indexes = []
        add_campaign_trace(args.project, args.args, symbolic_indexes, args.name)
    elif args.current_parser == "remove-trace":
        if args.name:
            name = int(args.name) if args.id else args.name
            remove_campaign_trace(args.project, name)
        elif args.all:
            remove_campaign_all_traces(args.project)
        else:
            parser.error("missing trace name or id")
    elif args.current_parser == "describe":
        describe_campaign(args.project)
    elif args.current_parser == "run":
        run_campaign(args.project)
    elif args.current_parser == "run-trace":
        if args.name:
            name = int(args.name) if args.id else args.name
        elif args.last:
            name = -1
        else:
            parser.error("missing trace name or id")

        run_campaign_trace(args.project, name)
    elif args.current_parser == "validate":
        validate_campaign(args.project)
    elif args.current_parser == "validate-trace":
        name = int(args.name) if args.id else args.name
        validate_campaign_trace(args.project, name)
    elif args.current_parser == "validate-args":
        validate_campaign_with_args(args.project, args.args)
    elif args.current_parser == "clear-trace-data":
        clear_project_trace_data(args.project)
    elif args.current_parser == "set-trace-stdin":
        name = int(args.name) if args.id else args.name
        set_trace_stdin(args.project, name, args.stdin)
    elif args.current_parser == "add-trace-input-file":
        name = int(args.name) if args.id else args.name
        dest = Path(args.destination) if args.destination else None
        permissions = args.permissions or True
        add_trace_input_file(args.project, name, args.source, dest, permissions)
    elif args.current_parser == "remove-trace-input-file":
        name = int(args.name) if args.id else args.name
        remove_trace_input_file(args.project, name, args.source)
    elif args.current_parser == "add-trace-setup":
        name = int(args.name) if args.id else args.name
        add_trace_setup(args.project, name, args.command)
    elif args.current_parser == "add-trace-teardown":
        name = int(args.name) if args.id else args.name
        add_trace_teardown(args.project, name, args.command)
    else:
        parser.print_help()


if __name__ == "__main__":  # pragma: no cover
    main()
