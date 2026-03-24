#!/usr/bin/env python3
"""
compile_metadata.py — GIP Metadata JSON → Binary Compiler

Ported from Microsoft's C# GIP Metadata Compiler (GIP Metadata Compiler.docx).
Produces the binary blob that the wheel sends in response to GIP command 0x04
(Identify/Metadata request).

Binary format (all little-endian):
  MetadataHeader (16 bytes):
    u16 header_size (always 16)
    u16 version_major
    u16 version_minor
    u16 reserved1-4 (zeros)
    u16 total_metadata_size

  DeviceMetadata (variable):
    u16 size
    u16 supported_devices_offset
    u16 audio_formats_offset
    u16 inbound_sys_msgs_offset
    u16 outbound_sys_msgs_offset
    u16 preferred_types_offset
    u16 supported_interfaces_offset
    u16 hid_descriptors_offset
    u16 reserved1-3
    [variable data sections, each prefixed with u8 count]

  u8 message_count
  MessageMetadata[] (each variable):
    u16 size
    u8  gip_message_type
    u16 gip_message_length
    u16 data_type (1=custom, 2=audio, 3=security, 4=gip)
    u32 flags
    u16 period
    u16 persistence_timeout
    u16 reserved1-4

Usage:
  python3 compile_metadata.py input.json [output.bin]
  python3 compile_metadata.py input.json --c-header  (outputs C header with const array)
"""

import json
import struct
import sys
import uuid


DATA_TYPE_MAP = {
    "custom": 1,
    "audio": 2,
    "security": 3,
    "gip": 4,
}


def compile_metadata_header(header_json, total_size):
    """MetadataHeader: 16 bytes."""
    return struct.pack("<HHH HHHH H",
        16,                             # header_size
        header_json["MajorVersion"],    # version_major
        header_json["MinorVersion"],    # version_minor
        0, 0, 0, 0,                    # reserved1-4
        total_size,                     # total_metadata_size
    )


def compile_device_metadata(dev_json):
    """DeviceMetadata: header (22 bytes) + variable data sections."""
    # Calculate sizes and offsets
    header_size = 2 * 11  # 11 x u16 fields = 22 bytes

    offset = header_size

    # Supported firmware versions
    versions = dev_json.get("SupportsDeviceFirmwareVersions", [])
    devices_offset = offset
    offset += 1  # count byte
    offset += len(versions) * 4  # each version is u16 major + u16 minor

    # Audio formats
    audio_formats = dev_json.get("SupportedAudioFormats", [])
    audio_offset = offset
    offset += 1  # count byte
    offset += len(audio_formats) * 2  # each is u8 inbound + u8 outbound

    # Inbound system messages
    in_msgs = dev_json.get("SupportedInSystemCommands", [])
    in_msgs_offset = offset
    offset += 1  # count byte
    offset += len(in_msgs)

    # Outbound system messages
    out_msgs = dev_json.get("SupportedOutSystemCommands", [])
    out_msgs_offset = offset
    offset += 1  # count byte
    offset += len(out_msgs)

    # Preferred types (length-prefixed ASCII strings)
    types = dev_json.get("PreferredTypes", [])
    types_offset = offset
    offset += 1  # count byte
    for t in types:
        offset += 2  # u16 string length
        offset += len(t)  # ASCII bytes (not null-terminated)

    # Supported interfaces (16-byte GUIDs)
    interfaces = dev_json.get("SupportedInterfaces", [])
    interfaces_offset = offset
    offset += 1  # count byte
    offset += len(interfaces) * 16  # each GUID is 16 bytes

    # HID descriptors (version 1.1+, we're 1.0 so skip)
    hid_offset = 0  # not used in v1.0

    total_size = offset

    # Build binary
    data = bytearray()

    # Header: 11 x u16
    data += struct.pack("<H", total_size)
    data += struct.pack("<H", devices_offset)
    data += struct.pack("<H", audio_offset)
    data += struct.pack("<H", in_msgs_offset)
    data += struct.pack("<H", out_msgs_offset)
    data += struct.pack("<H", types_offset)
    data += struct.pack("<H", interfaces_offset)
    data += struct.pack("<H", hid_offset)
    data += struct.pack("<H", 0)  # reserved1
    data += struct.pack("<H", 0)  # reserved2
    data += struct.pack("<H", 0)  # reserved3

    assert len(data) == header_size

    # Supported firmware versions
    data += struct.pack("B", len(versions))
    for v in versions:
        data += struct.pack("<HH", v["Major"], v["Minor"])

    # Audio formats
    data += struct.pack("B", len(audio_formats))
    for af in audio_formats:
        data += struct.pack("BB", af.get("Inbound", 0), af.get("Outbound", 0))

    # Inbound system messages
    data += struct.pack("B", len(in_msgs))
    for m in in_msgs:
        data += struct.pack("B", m)

    # Outbound system messages
    data += struct.pack("B", len(out_msgs))
    for m in out_msgs:
        data += struct.pack("B", m)

    # Preferred types
    data += struct.pack("B", len(types))
    for t in types:
        encoded = t.encode("ascii")
        data += struct.pack("<H", len(encoded))
        data += encoded

    # Supported interfaces (GUIDs in mixed-endian format per Microsoft convention)
    data += struct.pack("B", len(interfaces))
    for guid_str in interfaces:
        g = uuid.UUID(guid_str)
        # Microsoft GUID binary format: first 3 components LE, last 2 BE
        # uuid.UUID.bytes_le gives exactly this
        data += g.bytes_le

    assert len(data) == total_size
    return bytes(data)


def compile_message_flags(msg_json):
    """Convert message boolean flags to u32 bitfield."""
    flags = 0
    if msg_json.get("IsBigEndian", False):
        flags |= (1 << 0)
    if msg_json.get("IsReliable", False):
        flags |= (1 << 1)
    if msg_json.get("IsSequenced", False):
        flags |= (1 << 2)
    if msg_json.get("IsDownstream", False):
        flags |= (1 << 3)
    if msg_json.get("IsUpstream", False):
        flags |= (1 << 4)
    return flags


def compile_message_metadata(msg_json):
    """MessageMetadata: 22 bytes each."""
    data_type = DATA_TYPE_MAP[msg_json["DataType"].lower()]
    flags = compile_message_flags(msg_json)

    # Size: u16(2) + u8(1) + u16(2) + u16(2) + u32(4) + u16(2) + u16(2) + 4*u16(8) = 23
    msg_size = 23
    data = struct.pack("<H", msg_size)
    data += struct.pack("B", msg_json["MessageType"])
    data += struct.pack("<H", msg_json["MessageLength"])
    data += struct.pack("<H", data_type)
    data += struct.pack("<I", flags)
    data += struct.pack("<H", msg_json.get("Period", 0))
    data += struct.pack("<H", msg_json.get("PersistenceTimeout", 0))
    data += struct.pack("<H", 0)  # reserved1
    data += struct.pack("<H", 0)  # reserved2
    data += struct.pack("<H", 0)  # reserved3
    data += struct.pack("<H", 0)  # reserved4

    assert len(data) == msg_size
    return data


def compile_metadata(json_path):
    """Compile a GIP metadata JSON file to binary blob."""
    with open(json_path, "r") as f:
        meta = json.load(f)

    header_json = meta["MetadataHeader"]
    dev_json = meta["DeviceMetadata"]
    messages_json = meta["Messages"]

    # Compile sections
    dev_blob = compile_device_metadata(dev_json)
    msg_blobs = [compile_message_metadata(m) for m in messages_json]

    # Calculate total size
    total = 16  # metadata header
    total += len(dev_blob)
    total += 1  # message count byte
    for mb in msg_blobs:
        total += len(mb)

    # Build final blob
    blob = bytearray()
    blob += compile_metadata_header(header_json, total)
    blob += dev_blob
    blob += struct.pack("B", len(msg_blobs))
    for mb in msg_blobs:
        blob += mb

    assert len(blob) == total
    return bytes(blob)


def to_c_header(blob, var_name="gip_wheel_metadata"):
    """Convert binary blob to a C header file with const array."""
    lines = []
    lines.append(f"// Auto-generated by compile_metadata.py — do not edit")
    lines.append(f"// Total size: {len(blob)} bytes")
    lines.append(f"")
    lines.append(f"#ifndef GIP_WHEEL_METADATA_H")
    lines.append(f"#define GIP_WHEEL_METADATA_H")
    lines.append(f"")
    lines.append(f"#include <stdint.h>")
    lines.append(f"")
    lines.append(f"static const uint8_t {var_name}[] = {{")

    # Format as hex bytes, 16 per line
    for i in range(0, len(blob), 16):
        chunk = blob[i:i+16]
        hex_bytes = ", ".join(f"0x{b:02X}" for b in chunk)
        comma = "," if i + 16 < len(blob) else ""
        lines.append(f"    {hex_bytes}{comma}")

    lines.append(f"}};")
    lines.append(f"")
    lines.append(f"static const uint16_t {var_name}_len = sizeof({var_name});")
    lines.append(f"")
    lines.append(f"#endif // GIP_WHEEL_METADATA_H")

    return "\n".join(lines)


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} input.json [output.bin|--c-header]")
        sys.exit(1)

    json_path = sys.argv[1]
    blob = compile_metadata(json_path)

    if len(sys.argv) > 2 and sys.argv[2] == "--c-header":
        print(to_c_header(blob))
    elif len(sys.argv) > 2:
        out_path = sys.argv[2]
        with open(out_path, "wb") as f:
            f.write(blob)
        print(f"Wrote {len(blob)} bytes to {out_path}")
    else:
        out_path = json_path.rsplit(".", 1)[0] + ".bin"
        with open(out_path, "wb") as f:
            f.write(blob)
        print(f"Wrote {len(blob)} bytes to {out_path}")

    # Always print hex dump for verification
    print(f"\nHex dump ({len(blob)} bytes):")
    for i in range(0, len(blob), 16):
        hex_part = " ".join(f"{b:02X}" for b in blob[i:i+16])
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in blob[i:i+16])
        print(f"  {i:04X}: {hex_part:<48s} {ascii_part}")


if __name__ == "__main__":
    main()
