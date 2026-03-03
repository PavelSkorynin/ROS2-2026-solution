#!/usr/bin/env python3
"""
Generate an ArUco marker image using OpenCV.

Example:
  python3 generate_aruco.py --dict DICT_4X4_50 --id 0 --size 400 --output marker.png
"""

import argparse
import sys

import cv2


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate an ArUco marker image.")
    parser.add_argument("--dict", default="DICT_4X4_50", help="ArUco dictionary name.")
    parser.add_argument("--id", type=int, default=0, help="Marker ID within the dictionary.")
    parser.add_argument("--size", type=int, default=400, help="Marker image size in pixels.")
    parser.add_argument("--border", type=int, default=1, help="Border bits width.")
    parser.add_argument("--output", default="aruco_marker.png", help="Output image path.")
    return parser.parse_args()


def get_dictionary(dict_name: str):
    if not hasattr(cv2.aruco, dict_name):
        valid = [name for name in dir(cv2.aruco) if name.startswith("DICT_")]
        raise ValueError(f"Unknown dictionary '{dict_name}'. Valid: {', '.join(valid)}")
    dict_id = getattr(cv2.aruco, dict_name)
    return cv2.aruco.getPredefinedDictionary(dict_id)


def main() -> int:
    args = parse_args()
    if args.size <= 0:
        print("Error: --size must be > 0", file=sys.stderr)
        return 2
    if args.border < 0:
        print("Error: --border must be >= 0", file=sys.stderr)
        return 2

    aruco_dict = get_dictionary(args.dict)
    marker = cv2.aruco.generateImageMarker(aruco_dict, args.id, args.size, None, args.border)

    if not cv2.imwrite(args.output, marker):
        print(f"Error: failed to write '{args.output}'", file=sys.stderr)
        return 1

    print(f"Saved: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
