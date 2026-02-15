#!/usr/bin/env python3
"""
PTSL Client for creating tempo markers in Pro Tools
Called by the C++ application with bar data containing BPM information
"""

import sys
import os
import json
import argparse

# Add bundled py-ptsl to path if running from app bundle
script_dir = os.path.dirname(os.path.abspath(__file__))
if os.path.exists(os.path.join(script_dir, 'ptsl')):
    sys.path.insert(0, script_dir)
    # Also import setup_paths if it exists to configure bundled grpc
    setup_paths = os.path.join(script_dir, 'setup_paths.py')
    if os.path.exists(setup_paths):
        import importlib.util
        spec = importlib.util.spec_from_file_location("setup_paths", setup_paths)
        setup_module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(setup_module)

try:
    import ptsl
except ImportError as e:
    print(json.dumps({"success": False, "message": f"Failed to import py-ptsl: {e}"}))
    sys.exit(1)

def timecode_add_seconds(base_tc, seconds, frame_rate=30):
    """
    Add seconds to a timecode string
    
    Args:
        base_tc: Base timecode string (HH:MM:SS:FF)
        seconds: Seconds to add (float)
        frame_rate: Frame rate (default 30)
    
    Returns:
        New timecode string
    """
    # Parse base timecode
    parts = base_tc.split(':')
    hours = int(parts[0])
    minutes = int(parts[1])
    secs = int(parts[2])
    frames = int(parts[3])
    
    # Convert to total frames
    total_frames = (hours * 3600 + minutes * 60 + secs) * frame_rate + frames
    
    # Add the seconds as frames
    total_frames += int(seconds * frame_rate)
    
    # Convert back to timecode
    hours = total_frames // (3600 * frame_rate)
    remaining = total_frames % (3600 * frame_rate)
    minutes = remaining // (60 * frame_rate)
    remaining = remaining % (60 * frame_rate)
    secs = remaining // frame_rate
    frames = remaining % frame_rate
    
    return f"{hours:02d}:{minutes:02d}:{secs:02d}:{frames:02d}"

def get_frame_rate_from_timecode_rate(tc_rate):
    """
    Convert Pro Tools timecode rate enum to actual frame rate
    
    Pro Tools timecode rate values (from PTSL):
    1 = 23.976 fps
    2 = 24 fps
    3 = 25 fps (PAL)
    4 = 29.97 fps (drop frame)
    5 = 29.97 fps (non-drop)
    6 = 30 fps (non-drop)
    7 = 30 fps (drop frame)
    8 = 47.952 fps
    9 = 48 fps
    10 = 50 fps
    11 = 59.94 fps (drop frame)
    12 = 59.94 fps (non-drop)
    13 = 60 fps (drop frame)
    14 = 60 fps (non-drop)
    15 = 100 fps
    16 = 119.88 fps (drop frame)
    17 = 119.88 fps (non-drop)
    18 = 120 fps (drop frame)
    19 = 120 fps (non-drop)
    """
    rate_map = {
        1: 23.976, 2: 24, 3: 25, 4: 29.97, 5: 29.97, 6: 30, 7: 30,
        8: 47.952, 9: 48, 10: 50, 11: 59.94, 12: 59.94, 13: 60, 14: 60,
        15: 100, 16: 119.88, 17: 119.88, 18: 120, 19: 120
    }
    return rate_map.get(tc_rate, 30)

def create_bar_markers(bars_json, start_timecode="01:00:00:00", 
                      clear_existing=True,
                      company_name="Samuel Justice", app_name="PTSL Beat Tool"):
    """
    Create bar markers in Pro Tools with BPM labels
    
    Args:
        bars_json: JSON string containing bars array with time, bpm, and bar_number
        start_timecode: Starting timecode
        clear_existing: Clear existing memory locations
        company_name: Company name for PTSL registration
        app_name: Application name for PTSL registration
    
    Returns:
        JSON string with success status and message
    """
    try:
        # Parse bar data
        data = json.loads(bars_json)
        bars = data['bars']
        
        # Connect to Pro Tools
        try:
            with ptsl.open_engine(
                company_name=company_name,
                application_name=app_name
            ) as engine:
                
                # Get session info
                session_frame_rate = 30  # Default
                try:
                    session_name = engine.session_name()
                    tc_rate = engine.session_timecode_rate()
                    session_frame_rate = get_frame_rate_from_timecode_rate(tc_rate)
                    print(f"Session: {session_name}, Timecode Rate: {tc_rate}, Frame Rate: {session_frame_rate} fps", file=sys.stderr)
                except Exception as e:
                    print(f"Warning: Could not get session info ({str(e)}), using {session_frame_rate} fps", file=sys.stderr)
                
                # Clear existing memory locations if requested
                if clear_existing:
                    try:
                        # Get all memory locations and delete them
                        locations = engine.get_memory_locations()
                        # Note: py-ptsl doesn't have delete_memory_location, so we can't clear them
                        if locations:
                            print(f"Note: Found {len(locations)} existing memory locations (clearing not supported by py-ptsl)", file=sys.stderr)
                    except:
                        pass
                
                # Get existing memory locations to find the next available number
                existing_locations = []
                try:
                    existing_locations = engine.get_memory_locations()
                    existing_numbers = set()
                    for loc in existing_locations:
                        if hasattr(loc, 'number'):
                            existing_numbers.add(loc.number)
                    print(f"Found {len(existing_numbers)} existing memory locations", file=sys.stderr)
                except Exception as e:
                    print(f"Warning: Could not get existing locations: {e}", file=sys.stderr)
                    existing_numbers = set()
                
                # Create memory locations for bars
                created = 0
                next_number = 1
                for bar in bars:
                    # Find next available memory location number
                    while next_number in existing_numbers:
                        next_number += 1
                    
                    # Calculate timecode for this bar
                    time_seconds = bar['time']
                    bar_timecode = timecode_add_seconds(start_timecode, time_seconds, session_frame_rate)
                    
                    # Create marker name with BPM
                    bar_number = bar['bar_number']
                    bpm = bar['bpm']
                    name = f"Bar {bar_number} - {bpm:.1f} BPM"
                    
                    # Create memory location with explicit number
                    try:
                        engine.create_memory_location(
                            name=name,
                            start_time=bar_timecode,
                            memory_number=next_number
                        )
                        created += 1
                        existing_numbers.add(next_number)
                        next_number += 1
                        if created <= 5:  # Log first few markers
                            print(f"Created marker #{next_number-1}: {name} at {bar_timecode}", file=sys.stderr)
                    except Exception as e:
                        print(f"Warning: Failed to create marker {name}: {e}", file=sys.stderr)
                
                if created > 5:
                    print(f"... and {created - 5} more markers", file=sys.stderr)
                
                return json.dumps({
                    "success": True, 
                    "message": f"Created {created} bar markers in Pro Tools",
                    "markers_created": created
                })
                
        except Exception as e:
            return json.dumps({"success": False, "message": f"Failed to connect to Pro Tools: {str(e)}"})
            
    except Exception as e:
        return json.dumps({
            "success": False,
            "message": f"Error: {str(e)}"
        })

def main():
    """Main function with argument parsing"""
    parser = argparse.ArgumentParser(description='Create tempo markers in Pro Tools')
    parser.add_argument('--bars', action='store_true', help='Process bar data instead of beats')
    parser.add_argument('json_data', help='JSON data containing bars or beats (can be JSON string or file path)')
    parser.add_argument('start_timecode', help='Starting timecode')
    parser.add_argument('--clear', action='store_true', help='Clear existing markers')
    
    args = parser.parse_args()
    
    # Check if json_data is a file path or JSON string
    json_string = args.json_data
    if os.path.exists(args.json_data):
        # It's a file path, read the file
        try:
            with open(args.json_data, 'r') as f:
                json_string = f.read()
        except Exception as e:
            print(json.dumps({"success": False, "message": f"Failed to read JSON file: {str(e)}"}))
            sys.exit(1)
    
    if args.bars:
        # New bar-based format
        result = create_bar_markers(json_string, args.start_timecode, args.clear)
    else:
        # Legacy beat format (for backwards compatibility)
        print(json.dumps({"success": False, "message": "Beat format no longer supported. Use --bars flag."}))
        sys.exit(1)
    
    print(result)

if __name__ == "__main__":
    main()