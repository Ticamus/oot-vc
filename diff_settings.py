import re
from pathlib import Path

def add_custom_arguments(parser):
    parser.add_argument("-v", "--version", help="Emulator version to diff", default="oot-j")

def apply(config, args):
    version = args.version
    name_match = re.search(r"^name: *(\S+)", Path(f"config/{version}/config.yml").read_text(), re.MULTILINE)
    name = name_match.group(1)
    config["make_command"] = ["ninja"]
    config["mapfile"] = f"build/{version}/{name}.elf.MAP"
    config["source_directories"] = ["src", "include", "libc", f"build/{version}/include"]
    config["arch"] = "ppc"
    config["map_format"] = "mw" # gnu, mw, ms
    config["build_dir"] = f"build/{version}/src" # only needed for mw and ms map formats
    config["objdump_executable"] = "build/binutils/powerpc-eabi-objdump"
    config["show_line_numbers_default"] = True
