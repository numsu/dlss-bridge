#!/usr/bin/env python3
from pathlib import Path
import sys
VALUES={'VulkanUpscaler':'dlss','Enabled':'true','ToggleKey':'0x87','Passes':'1','WorkingScale':'1.0','FinishedPicture':'false','ResidualAcrossRR':'false','ResidualFG':'false','AutoCapture':'false','RunBeforeSR':'true','DeferredDLSS':'false'}
if len(sys.argv)!=2: raise SystemExit(f'usage: {sys.argv[0]} OptiScaler.ini')
p=Path(sys.argv[1]); lines=p.read_text(encoding='utf-8-sig').splitlines(); section=''; seen=set(); out=[]
for line in lines:
 s=line.strip()
 if s.startswith('[') and s.endswith(']'): section=s[1:-1].lower()
 key=s.split('=',1)[0] if '=' in s and not s.startswith(';') else ''
 value=VALUES.get(key) if key=='VulkanUpscaler' or section=='dlssnr' else None
 if value is None: out.append(line)
 else: out.append(f'{key}={value}'); seen.add(key)
missing=set(VALUES)-seen
if missing: raise SystemExit('OptiScaler.ini lacks expected settings: '+', '.join(sorted(missing)))
p.write_text('\n'.join(out)+'\n',encoding='utf-8')
