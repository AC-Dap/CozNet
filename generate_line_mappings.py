import argparse
import sys
import os
import posixpath
import pandas as pd
from elftools.elf.elffile import ELFFile

def get_text_section_base_address(elffile):
    """
    Attempts to find the base address of the .text section.
    This is often 0 for PIE/shared objects in the ELF file itself,
    representing an offset.
    For non-PIE executables, it might be the actual link address.
    For our purposes (getting offsets), a section's sh_addr is what we need.
    """
    text_section = elffile.get_section_by_name('.text')
    if text_section:
        return text_section['sh_addr']
    return 0 # Default if .text section not found or other issue

def generate_line_to_offset_mapping(filepath, output_filepath=None, module_name=None):
    """
    Generates a mapping from source lines to instruction offsets for an ELF file.

    :param filepath: Path to the ELF binary or shared library.
    :param output_filepath: Optional path to save the mapping. If None, prints to stdout.
    :param module_name: Optional name of the module (e.g., libraft.so).
                       If None, the basename of filepath is used.
    """
    mappings = []
    if module_name is None:
        module_name = os.path.basename(filepath)

    try:
        with open(filepath, 'rb') as f:
            elffile = ELFFile(f)

            if not elffile.has_dwarf_info():
                print(f"Error: File '{filepath}' has no DWARF info. Compile with -g.", file=sys.stderr)
                return

            print(f"Dumping line mappings for {filepath}.")
            dwarfinfo = elffile.get_dwarf_info()
            # text_section_base = get_text_section_base_address(elffile) Used to confirm addresses are within .text if needed

            for CU in dwarfinfo.iter_CUs():
                print('Found a compile unit at offset %s, length %s' % (
                    CU.cu_offset, CU['unit_length']))

                # The line program provides the mapping from code addresses to source lines.
                lineprog = dwarfinfo.line_program_for_CU(CU)
                if lineprog is None:
                    print('  DWARF info is missing a line program for this CU')
                    continue

                # CU.header.file_table is a list of FNDE (File Name Decl Entry) objects
                # The first entry (index 0) is usually the compilation directory,
                # subsequent entries (index 1 onwards) are the source files.
                # FNDE attributes:
                #   name (str): The filename.
                #   dir_index (int): Index into the list of include directories for this CU.
                #   mtime (int): Modification time.
                #   length (int): File length.
                lp_header = lineprog.header # This is the LineProgramHeader object
                file_entries = lp_header["file_entry"]

                # Decode the line program entries
                for entry in lineprog.get_entries():
                    # We're interested in entries that are not end_sequence and have a valid line number
                    # An entry associates an address with a source line.
                    if entry.state is None or entry.state.end_sequence:
                        continue

                    # entry.state.file is an index into the CU's file table.
                    # File index 0 is usually the compilation directory or a placeholder.
                    # Valid file indices start from 1.
                    file_index = entry.state.file
                    if file_index == 0 or file_index > len(file_entries):
                        # This can happen for compiler-generated code or prologue/epilogue
                        # without explicit source lines.
                        print(f"Skipping entry with file_index {file_index} at address 0x{entry.state.address:x}", file=sys.stderr)
                        continue

                    # File and directory indices are 1-indexed in DWARF version < 5,
                    # 0-indexed in DWARF5.
                    if lp_header.version < 5:
                        file_index -= 1
                        if file_index == -1:
                            continue
                        file_entry = file_entries[file_index]
                        dir_index = file_entry["dir_index"] - 1
                        filename = file_entry.name if dir_index == -1 else posixpath.join(lp_header["include_directory"][dir_index], file_entry.name)
                        filename = filename.decode('utf-8', errors='replace')
                    else:
                        file_entry = file_entries[file_index]
                        dir_index = file_entry["dir_index"]
                        filename = posixpath.join(lp_header["include_directory"][dir_index], file_entry.name).decode('utf-8', errors='replace')

                    line_num = entry.state.line
                    address_offset = entry.state.address # This is the VMA (Virtual Memory Address)

                    # We only want lines that are not marked as "prologue end" or "epilogue begin"
                    # and are actual statements (not basic blocks etc., though lineprog doesn't distinguish that finely)
                    # is_stmt is a boolean indicating if the instruction is recommended as a statement boundary.
                    if entry.state.is_stmt:
                        # Ensure the address is within a reasonable range (e.g., .text section) if desired.
                        # For simplicity, we'll take all addresses from the line program.
                        # The 'address_offset' is usually the absolute VMA as linked in the ELF.
                        # For PIE/shared libs, this is an offset from the load base.
                        mappings.append({
                            "module": module_name,
                            "file": filename,
                            "line": line_num,
                            "offset": address_offset # This is the VMA, effectively an offset for PIE/shared
                        })

    except FileNotFoundError:
        print(f"Error: File '{filepath}' not found.", file=sys.stderr)
        return
    except Exception as e:
        print(f"An error occurred while processing '{filepath}': {e}", file=sys.stderr)
        return

    # Deduplicate and sort (optional, but makes output cleaner)
    # A single source line can map to multiple instruction blocks, but we often just need one offset per line.
    # However, for *random selection of an executable instruction*, more entries are better.
    # For now, let's keep all direct mappings from line program entries.
    # You might want to refine what constitutes a "unique mappable line" later.

    # Output
    df = pd.DataFrame(mappings)
    df['offset'] = df['offset'].apply(lambda x: hex(x))

    if output_filepath:
        df.to_csv(output_filepath, header=False, index=False)
    else:
        with pd.option_context('display.max_rows', None, 'display.max_columns', None):
            print(df)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(prog='Generate Line Mappings')
    parser.add_argument('-o', '--out', help="File to put mappings in. If not given, prints to stdout.", default=None)
    parser.add_argument('-m', '--module', help="What module name this binary corresponds to", default=None)
    parser.add_argument("elf_path", help="The binary to generate mappings for")

    args = parser.parse_args()

    generate_line_to_offset_mapping(args.elf_path, args.out, args.module)