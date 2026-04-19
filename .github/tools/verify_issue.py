#!/usr/bin/env python3
"""Verify the GitHub issue was created successfully"""

import urllib.request
import json

try:
    url = "https://api.github.com/repos/erni4711/MyESPhome/issues/1"
    req = urllib.request.Request(url, headers={"Accept": "application/vnd.github.v3+json"})
    
    with urllib.request.urlopen(req, timeout=10) as resp:
        data = json.loads(resp.read().decode('utf-8'))
        
        print("✅ Issue verified on GitHub!")
        print(f"   Issue #: {data['number']}")
        print(f"   Title: {data['title'][:60]}...")
        print(f"   State: {data['state']}")
        print(f"   URL: {data['html_url']}")
        labels = [l['name'] for l in data['labels']]
        if labels:
            print(f"   Labels: {', '.join(labels)}")
        
except Exception as e:
    print(f"❌ Error verifying issue: {e}")
    exit(1)
