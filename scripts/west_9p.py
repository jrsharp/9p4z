#!/usr/bin/env python3
# Copyright (c) 2025 9p4z Contributors
# SPDX-License-Identifier: Apache-2.0

"""West extension commands for 9P development"""

import argparse
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

from west.commands import WestCommand


class NinePServe(WestCommand):
    def __init__(self):
        super().__init__(
            '9p-serve',
            'start 9P file server',
            'Start 9pserve with 9pex to serve a directory over 9P')

    def do_add_parser(self, parser_adder):
        parser = parser_adder.add_parser(
            self.name,
            help=self.help,
            description=self.description)

        parser.add_argument(
            'directory',
            nargs='?',
            default=os.getcwd(),
            help='directory to serve (default: current directory)')

        parser.add_argument(
            '-s', '--socket',
            default='/tmp/9p.sock',
            help='Unix socket path (default: /tmp/9p.sock)')

        parser.add_argument(
            '-p', '--port',
            type=int,
            help='TCP port to listen on (instead of Unix socket)')

        parser.add_argument(
            '-d', '--daemon',
            action='store_true',
            help='run in background')

        return parser

    def do_run(self, args, unknown_args):
        directory = Path(args.directory).resolve()

        if not directory.is_dir():
            self.die(f"Directory not found: {directory}")

        # Check for 9pex and 9pserve
        if not self._check_command('9pex'):
            self.die("9pex not found. Install plan9port (brew install plan9port)")
        if not self._check_command('9pserve'):
            self.die("9pserve not found. Install plan9port (brew install plan9port)")

        # Build command
        if args.port:
            addr = f"tcp!*!{args.port}"
            print(f"Starting 9P server on TCP port {args.port}...")
        else:
            addr = f"unix!{args.socket}"
            # Remove existing socket
            if os.path.exists(args.socket):
                os.unlink(args.socket)
            print(f"Starting 9P server on Unix socket {args.socket}...")

        cmd = f"9pex {directory} | 9pserve {addr}"

        if args.daemon:
            cmd += " &"
            print(f"Command: {cmd}")
            os.system(cmd)
            time.sleep(0.5)
            print("Server started in background")
            if not args.port:
                print(f"Socket: {args.socket}")
        else:
            print(f"Command: {cmd}")
            print("Press Ctrl+C to stop server")
            try:
                os.system(cmd)
            except KeyboardInterrupt:
                print("\nServer stopped")

    def _check_command(self, cmd):
        """Check if command exists"""
        return subprocess.run(['which', cmd],
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL).returncode == 0


class NinePRun(WestCommand):
    def __init__(self):
        super().__init__(
            '9p-run',
            'run QEMU with 9P client',
            'Run QEMU with 9P client sample connected to 9pserve')

    def do_add_parser(self, parser_adder):
        parser = parser_adder.add_parser(
            self.name,
            help=self.help,
            description=self.description)

        parser.add_argument(
            '-s', '--socket',
            default='/tmp/9p.sock',
            help='Unix socket path to connect to (default: /tmp/9p.sock)')

        parser.add_argument(
            '-b', '--board',
            default='qemu_x86',
            help='board to run (default: qemu_x86)')

        parser.add_argument(
            '-d', '--build-dir',
            help='build directory (default: build)')

        parser.add_argument(
            '--serve-dir',
            help='if specified, automatically start 9pserve on this directory')

        parser.add_argument(
            '-m', '--memory',
            type=int,
            default=32,
            help='QEMU memory in MB (default: 32)')

        return parser

    def do_run(self, args, unknown_args):
        # Auto-start server if requested
        server_proc = None
        if args.serve_dir:
            serve_dir = Path(args.serve_dir).resolve()
            if not serve_dir.is_dir():
                self.die(f"Serve directory not found: {serve_dir}")

            # Remove existing socket
            if os.path.exists(args.socket):
                os.unlink(args.socket)

            print(f"Starting 9P server for {serve_dir}...")
            server_proc = subprocess.Popen(
                f"9pex {serve_dir} | 9pserve unix!{args.socket}",
                shell=True,
                preexec_fn=os.setsid)
            time.sleep(0.5)

        # Check socket exists
        if not os.path.exists(args.socket):
            if server_proc:
                server_proc.terminate()
            self.die(f"Socket not found: {args.socket}\n"
                    f"Start server first with: west 9p-serve")

        # Find build directory
        if args.build_dir:
            build_dir = Path(args.build_dir)
        else:
            build_dir = Path('build')

        kernel = build_dir / 'zephyr' / 'zephyr.elf'
        if not kernel.exists():
            if server_proc:
                server_proc.terminate()
            self.die(f"Kernel not found: {kernel}\n"
                    f"Build first with: west build -b {args.board} 9p4z/samples/9p_client")

        # Build QEMU command
        cmd = [
            'qemu-system-i386',
            '-m', str(args.memory),
            '-cpu', 'qemu32',
            '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04',
            '-no-reboot',
            '-nographic',
            '-serial', f'unix:{args.socket}',
            '-kernel', str(kernel)
        ]

        print(f"Running QEMU with 9P client...")
        print(f"Command: {' '.join(cmd)}")
        print()

        try:
            subprocess.run(cmd)
        except KeyboardInterrupt:
            print("\nQEMU stopped")
        finally:
            if server_proc:
                print("Stopping 9P server...")
                os.killpg(os.getpgid(server_proc.pid), signal.SIGTERM)
