#!/usr/bin/env python3
"""Create GitHub issue for sd_file_server initialization problem"""

import subprocess
import json
import urllib.request
import urllib.error
import sys
import os

# Try to get GitHub token from environment or git config
token = os.environ.get('GITHUB_TOKEN')

if not token:
    try:
        result = subprocess.run(
            ['git', 'config', '--global', 'github.token'],
            capture_output=True,
            text=True,
            timeout=5
        )
        token = result.stdout.strip()
    except:
        pass

if not token:
    print("⚠️  GitHub token not found in environment or git config")
    print("   Please set GITHUB_TOKEN environment variable or run:")
    print("   git config --global github.token <your_token>")
    print("\n   Attempting to continue without authentication (may be rate-limited)...")

issue_title = "sd_file_server component not initializing HTTP handlers - /file endpoint times out"

issue_body = """## Problem Summary
The `/file` HTTP endpoint provided by the `sd_file_server` component times out (5-second timeout with no response). The device is otherwise fully operational (MQTT data flowing, sensors working, web_server_base responding to other endpoints).

## Root Cause Analysis
Through diagnostic investigation:
1. Build compilation completes successfully 
2. Device boots cleanly without panics
3. Boot logs show **NO sd_file_server initialization messages** even after 60+ seconds
4. When testing HTTP GET to http://192.168.10.233/file, the request times out

**The root cause is: `web_server_base::global_web_server_base` is nullptr when `sd_file_server::setup()` is called.**

The setup() method in `config/components/sd_file_server/sd_file_server.cpp` (lines 306-313) checks:
```cpp
if (web_server_base::global_web_server_base != nullptr) {
    web_server_base::global_web_server_base->add_handler(this);
} else {
    ESP_LOGW(TAG, "web_server_base not available; SD file server handler not registered");
}
```

The handler is **never registered** because `global_web_server_base` is null during setup.

## Environment
- Device: ESP32-P4 (EVBoard)
- ESPHome: 2026.4.0
- Board Config: ESP32-P4-WIFI6-Touch-LCD-7B.yaml
- Components: web_server_base, sd_file_server, sd_mmc_card

## Expected Behavior
- `/file` HTTP endpoint should be responsive
- HTTP GET requests should list files and allow downloads/uploads
- Handler should register during component initialization

## Actual Behavior  
- `/file` HTTP endpoint times out (5 seconds)
- Handler never registers (`web_server_base::global_web_server_base` is null)
- No error logs indicate the failure

## Diagnostic Evidence
✅ Board YAML config verified correct (sd_file_server section present)
✅ Python __init__.py has sd_file_server imports and schema
✅ C++ code compiles without errors
❌ Runtime: Handler registration code path not executed due to null global_web_server_base

## Potential Solutions
1. **Verify initialization order**: Check if web_server_base is initialized before sd_file_server's setup() is called
2. **Add dependency constraint**: Ensure sd_file_server setup() is called after web_server_base setup()
3. **Lazy initialization**: Defer handler registration to a later phase if dependencies aren't ready
4. **Additional diagnostic logging**: Add logging to understand why global_web_server_base is null

## Steps to Reproduce
1. Build firmware with sd_file_server component enabled
2. Flash to ESP32-P4 device
3. Boot device and wait for initialization complete
4. Attempt HTTP GET to /file endpoint
5. Observe timeout (no response after 5 seconds)
"""

headers = {
    "Accept": "application/vnd.github.v3+json",
    "User-Agent": "MyESPhome-Issue-Creator"
}

if token:
    headers["Authorization"] = f"token {token}"

data = {
    "title": issue_title,
    "body": issue_body,
    "labels": ["bug", "sd_file_server"]
}

url = "https://api.github.com/repos/erni4711/MyESPhome/issues"

try:
    req = urllib.request.Request(
        url,
        data=json.dumps(data).encode('utf-8'),
        headers=headers,
        method='POST'
    )
    
    with urllib.request.urlopen(req, timeout=10) as response:
        result = json.loads(response.read().decode('utf-8'))
        issue_number = result.get('number')
        issue_url = result.get('html_url')
        print(f"✅ Issue created successfully!")
        print(f"   Issue #: {issue_number}")
        print(f"   URL: {issue_url}")
        sys.exit(0)
        
except urllib.error.HTTPError as e:
    if e.code == 401:
        print(f"❌ Authentication failed (401). Please set a valid GitHub token.")
        print(f"   Error: {e.reason}")
    elif e.code == 403:
        print(f"❌ Access forbidden (403). Check token permissions.")
        print(f"   Error: {e.reason}")
    else:
        error_body = e.read().decode('utf-8')
        print(f"❌ GitHub API error ({e.code}): {e.reason}")
        try:
            error_data = json.loads(error_body)
            if 'message' in error_data:
                print(f"   Message: {error_data['message']}")
        except:
            print(f"   Response: {error_body[:200]}")
    sys.exit(1)
    
except Exception as e:
    print(f"❌ Error creating issue: {e}")
    sys.exit(1)
