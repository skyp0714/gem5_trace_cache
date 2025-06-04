#!/usr/bin/env python3

import argparse
import math
import random

import numpy as np


def generate_trace(num_pages, avg_interval, total_length, output_file):
    """
    Generate a CXL trace file with random parameters

    Args:
        num_pages: Number of unique pages to access
        avg_interval: Average time interval between requests in microseconds
        total_length: Total number of requests to generate
        output_file: Output file path
    """
    # Create a list of page addresses (aligned to 4KB)
    # Modified to work within 64GB range (0x000000000000 ~ 0x1000000000)
    # 4KB pages = 0x1000 bytes per page, so page addresses are 0x1000-aligned
    # Maximum page number in 64GB: 0x1000000000 / 0x1000 = 0x1000000
    pages = [
        random.randint(0, 0x8000000 - 1) * 0x1000 for _ in range(num_pages)
    ]

    # Open the output file
    with open(output_file, "w") as f:
        # Write the header
        f.write("// CXL Trace automatically generated\n")
        f.write(
            "// Format: <R/W> <Address(hex)> <Time(us)> <CompressionRatio(%)>\n"
        )
        f.write(
            f"// Parameters: {num_pages} pages, {avg_interval} us avg interval, {total_length} requests\n\n"
        )

        # Generate timestamps
        # Use exponential distribution to simulate realistic arrival patterns
        intervals = np.random.exponential(
            scale=avg_interval, size=total_length
        )
        timestamps = np.cumsum(intervals)

        # Sort timestamps to ensure they're in ascending order
        timestamps.sort()

        # Generate requests
        for i in range(total_length):
            # Always generate read requests (R)
            req_type = "R"

            # Randomly select a page and then a cache line within the page
            page = random.choice(pages)
            cache_line_offset = (
                random.randint(0, 63) * 64
            )  # 64 cache lines per 4KB page, 64 bytes per line
            address = page + cache_line_offset

            # Random compression ratio between 45% and 55%
            compression_ratio = round(random.uniform(45.0, 55.0), 1)

            # Get timestamp (in microseconds)
            timestamp = round(timestamps[i], 3)

            # Write the request to the file
            f.write(
                f"{req_type} 0x{address:x} {timestamp:.3f} {compression_ratio:.1f}\n"
            )

    print(f"Generated {total_length} trace requests in {output_file}")
    print(f"Time range: 0.0 to {timestamps[-1]:.3f} microseconds")


def main():
    # Set up command line arguments
    parser = argparse.ArgumentParser(
        description="Generate a random CXL trace file"
    )
    parser.add_argument(
        "-p",
        "--pages",
        type=int,
        default=500,
        help="Number of unique pages to access",
    )
    parser.add_argument(
        "-i",
        "--interval",
        type=float,
        default=0.03,
        help="Average interval between requests in microseconds",
    )
    parser.add_argument(
        "-l",
        "--length",
        type=int,
        default=100000,
        help="Total number of requests to generate",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=str,
        default="/home/hnpark2/traceCache/gem5/src/cxl_objects/trace/generated_trace.txt",
        help="Output file path",
    )

    args = parser.parse_args()

    # Call the trace generation function
    generate_trace(args.pages, args.interval, args.length, args.output)


if __name__ == "__main__":
    main()
