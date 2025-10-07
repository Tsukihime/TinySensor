import struct
import sys

def calculate_checksum(length, address, record_type, data_bytes):
    """
    Calculate the Intel HEX checksum for a record.
    
    :param length: Number of data bytes (int).
    :param address: 16-bit starting address (int).
    :param record_type: Record type (int, e.g., 0x00 for data).
    :param data_bytes: List of data byte values (list of int).
    :return: Checksum byte (int, 0–255).
    """
    checksum = length + ((address >> 8) & 0xFF) + (address & 0xFF) + record_type
    for byte in data_bytes:
        checksum += byte
    return (-checksum) & 0xFF

def generate_intel_hex_block(struct_bytes, address=0x0000):
    """
    Generate a single Intel HEX data record for the given bytes.
    
    :param struct_bytes: Packed structure bytes (bytes).
    :param address: Starting address for the record (int, default 0x0000).
    :return: List of HEX lines (list of str), including EOF.
    """
    data_list = list(struct_bytes)  # Convert to list of ints for checksum
    data_len = len(struct_bytes)
    
    # Calculate checksum
    checksum = calculate_checksum(data_len, address, 0x00, data_list)
    
    # Format data bytes as uppercase hex string
    data_hex = ''.join(f'{byte:02X}' for byte in data_list)
    
    # Assemble the data record line: :LLAAAATT[DD...]CC
    record_line = f":{data_len:02X}{address:04X}00{data_hex}{checksum:02X}"
    
    # Add End-of-File record
    eof_line = ":00000001FF"
    
    return [record_line, eof_line]

def main():
    """
    Main function to interactively collect user input, pack the SETTINGS struct,
    generate the Intel HEX file, and save it to 'settings_<bandgap>_<id>.eep'.
    """
    print("Sensor SETTINGS Structure Generator")
    print("==========================")
    print("This tool generates an Intel HEX file for the SETTINGS EEPROM struct.")
    print("Struct format: magic (0xC0DE), bandgap (uint16_t), power (uint8_t), id (char[6]).")
    print("Output: 11-byte data block at address 0x0000 + EOF record.\n")
    
    # Collect bandgap input (default: 1100)
    bandgap_input = input("Enter bandgap value (default: 1100, range: 0–65535): ").strip()
    if not bandgap_input:
        bandgap = 1100
    else:
        try:
            bandgap = int(bandgap_input)
            if not (0 <= bandgap <= 0xFFFF):
                print("Error: Bandgap must be between 0 and 65535.", file=sys.stderr)
                return 1
        except ValueError:
            print("Error: Bandgap must be a valid integer.", file=sys.stderr)
            return 1
    print(f"Bandgap set to: {bandgap} (0x{bandgap:04X})")
    
    # Collect power input (default: MAX=3)
    power_map = {"MIN": 0, "LOW": 1, "HIGH": 2, "MAX": 3}
    power_input = input("Enter power value (default: MAX, options: MIN=0, LOW=1, HIGH=2, MAX=3): ").strip().upper()
    if not power_input:
        power = 3  # Default: MAX
    else:
        if power_input in power_map:
            power = power_map[power_input]
        else:
            try:
                power = int(power_input)
                if not (0 <= power <= 3):
                    print("Error: Power must be between 0 and 3.", file=sys.stderr)
                    return 1
            except ValueError:
                print("Error: Power must be a valid integer (0–3) or one of MIN, LOW, HIGH, MAX.", file=sys.stderr)
                return 1
    print(f"Power set to: {power} ({list(power_map.keys())[list(power_map.values()).index(power)]})")
    
    # Collect id input (exactly 6 ASCII characters)
    id_input = input("Enter id (exactly 6 ASCII characters): ").strip()
    if len(id_input) != 6:
        print("Error: ID must be exactly 6 characters.", file=sys.stderr)
        return 1
    # Ensure ASCII (simple check)
    try:
        id_bytes = id_input.encode('ascii')
    except UnicodeEncodeError:
        print("Error: ID must contain only ASCII characters.", file=sys.stderr)
        return 1
    print(f"ID set to: '{id_input}'")
    
    # Pack the SETTINGS struct in little-endian format: <HHB6s
    # magic=0xC0DE (fixed), bandgap (uint16), power (uint8), id (6 bytes)
    try:
        struct_bytes = struct.pack("<HHB6s", 0xC0DE, bandgap, power, id_bytes)
    except struct.error as e:
        print(f"Error packing struct: {e}", file=sys.stderr)
        return 1
    
    # Generate HEX lines (11-byte block + EOF)
    hex_lines = generate_intel_hex_block(struct_bytes)
    
    # Generate output filename: settings_<bandgap>_<id>_<power>.eep
    output_file = f"settings_{bandgap}_{id_input}_{list(power_map.keys())[list(power_map.values()).index(power)]}.eep"
    
    # Write to file
    try:
        with open(output_file, "w") as f:
            for line in hex_lines:
                f.write(line + "\n")
        print(f"\nSuccess: Intel HEX file generated as '{output_file}'.")
        print("File contents:")
        for line in hex_lines:
            print(f"  {line}")
    except IOError as e:
        print(f"Error writing file '{output_file}': {e}", file=sys.stderr)
        return 1
    
    return 0

if __name__ == "__main__":
    sys.exit(main())
