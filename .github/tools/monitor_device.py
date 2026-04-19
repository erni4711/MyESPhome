import serial
import sys

try:
    ser = serial.Serial('COM7', 115200, timeout=1)
    print(f"Connected to {ser.port} at {ser.baudrate} baud", flush=True)
    
    line_count = 0
    while line_count < 300:  # Capture first 300 lines
        try:
            data = ser.readline()
            if data:
                decoded = data.decode('utf-8', errors='replace').rstrip('\r\n')
                if decoded:
                    print(decoded, flush=True)
                    line_count += 1
                    # Look for sd_file_server messages
                    if 'sd_file_server' in decoded:
                        print(f">>> FOUND SD_FILE_SERVER MESSAGE <<<", flush=True)
        except Exception as e:
            print(f"Error reading: {e}", flush=True)
            break
    
    ser.close()
    print("Monitor complete", flush=True)
except Exception as e:
    print(f"Error: {e}", flush=True)
