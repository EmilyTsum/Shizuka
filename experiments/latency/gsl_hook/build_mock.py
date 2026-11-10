#!/usr/bin/env python3
"""Build the pure C/ARM64 mock executable; no vendor library is needed or loaded."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path
from build_payload import DEFAULT_NDK, external_output

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ndk-bin',type=Path,default=DEFAULT_NDK)
    p.add_argument('--host',action='store_true')
    p.add_argument('--output',type=Path,required=True,
                   help='output executable outside the source checkout')
    args=p.parse_args();base=Path(__file__).parent
    if not args.host and args.ndk_bin is None:
        p.error('--ndk-bin or SHIZUKA_NDK_BIN is required')
    sources=[base/'payload_mock.c',base/'gsl_passthrough.c']
    command=['clang' if args.host else str(args.ndk_bin/'aarch64-linux-android34-clang'),
        '-std=c11','-O2','-Wall','-Wextra','-Werror']
    output=external_output(p,args.output)
    if not args.host:
        sources += [base/'gateway.S',base/'mock_caller.S']
        command += ['-mbranch-protection=standard','-Wl,-z,max-page-size=16384']
    command += [str(s) for s in sources]+['-o',str(output)]
    subprocess.run(command,check=True)
    metadata={'command':command,'sha256':hashlib.sha256(output.read_bytes()).hexdigest(),
        'sources_sha256':{str(s):hashlib.sha256(s.read_bytes()).hexdigest() for s in sources},
        'gpr_linked':False,'vendor_library_loaded':False,'device_executed':False}
    output.with_suffix('.build.json').write_text(json.dumps(metadata,indent=2)+'\n')
    if args.host:subprocess.run([str(output)],check=True)
    print(json.dumps(metadata,indent=2))

if __name__=='__main__':main()
