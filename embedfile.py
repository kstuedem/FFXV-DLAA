import argparse
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('in_file')
parser.add_argument('header_output')
args = parser.parse_args()

with open(args.in_file, 'rb') as f_in:
    data = f_in.read()
    
name = Path(args.header_output).stem
    
with open(args.header_output, 'w') as f_out:
    f_out.write('#pragma once\n')
    f_out.write(f'#ifndef __{name.upper()}_EMBED_FILE\n')
    f_out.write(f'#define __{name.upper()}_EMBED_FILE\n')
    f_out.write('#include <cstdint>\n')
    f_out.write('#include <span>\n')
    f_out.write(f'inline constexpr std::uint8_t __{name}_base[] = {{\n    ')
    
    line_length = 0
    for byte in data:
        f_out.write(str(byte) + ',')
        line_length += 1
        if line_length == 8:
            f_out.write('\n    ')
            line_length = 0
        else:
            f_out.write(' ')
    
    f_out.write('};\n')
    f_out.write(f'inline constexpr std::span<const std::uint8_t> __{name}{{\n');
    f_out.write(f'__{name}_base\n')
    f_out.write('};\n')
    f_out.write('#endif\n')