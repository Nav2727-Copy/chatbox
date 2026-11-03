#!/usr/bin/env python3
"""Linux/macOS PTY smoke: run after building, with permission for loopback sockets."""
import fcntl
import os
import pty
import select
import signal
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import termios
import time

binary = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else 'out/build/linux-debug/chatbox')

class Terminal:
    def __init__(self, directory):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 100, 0, 0))
        self.process = subprocess.Popen([binary], stdin=slave, stdout=slave, stderr=slave,
            cwd=directory, env=dict(os.environ, TERM='xterm-256color', XDG_DATA_HOME=directory))
        os.close(slave)
        self.read()
    def read(self, duration=.3):
        output = b''
        end = time.monotonic() + duration
        while time.monotonic() < end:
            if select.select([self.master], [], [], .03)[0]:
                try: output += os.read(self.master, 65536)
                except OSError: break
        return output
    def send(self, keys, duration=.3):
        os.write(self.master, keys)
        return self.read(duration)
    def resize(self):
        fcntl.ioctl(self.master, termios.TIOCSWINSZ, struct.pack('HHHH', 12, 40, 0, 0))
        self.process.send_signal(signal.SIGWINCH)
        return self.read()
    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        os.close(self.master)

with tempfile.TemporaryDirectory(prefix='chatbox-smoke-') as root:
    server_dir, client_dir = root + '/server', root + '/client'
    os.mkdir(server_dir); os.mkdir(client_dir)
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0)); port = listener.getsockname()[1]
    def start_server():
        process = subprocess.Popen([binary, '--server', str(port), '--no-upnp', '--allow-plaintext'],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, cwd=server_dir,
            env=dict(os.environ, XDG_DATA_HOME=server_dir))
        time.sleep(.2)
        assert process.poll() is None, process.stderr.read().decode()
        return process
    def stop_server(process):
        process.communicate(b'/quit\n', timeout=4)
        assert process.returncode == 0
    server = start_server()
    terminal = Terminal(client_dir)
    try:
        terminal.send(b'c'); terminal.send(b'smokeguest\ny\n'); terminal.send(b'i')
        output = terminal.send(f'chatbox:1|127.0.0.1|{port}|0|0|\n'.encode())
        assert b'Review' in output and b'INSECURE plaintext' in output, output
        terminal.send(b'\n'); terminal.send(b'j', 1)
        terminal.send(b'draft-kept')
        terminal.resize()
        stop_server(server)
        output = terminal.read(.5)
        assert b'Reconnect' in output, output
        server = start_server()
        output = terminal.read(3)
        assert b'draft-kept' in output, output
        # Reconnected session's details are accessible without losing the draft.
        output = terminal.send(b'\x1bOR') # xterm F3
        assert b'Connection / invite' in output, output
        terminal.send(b'\x1b')
        terminal.send(b'\x1b'); output = terminal.send(b'\n')
        assert b'Hello' in output, output
        terminal.send(b'q')
        assert terminal.process.wait(timeout=3) == 0
        db = sqlite3.connect(client_dir + '/chatbox/preferences.db')
        assert db.execute('select host,port,tls from recent').fetchone() == ('127.0.0.1', port, 0)
        db.close()
    finally:
        terminal.close()
        if server.poll() is None: stop_server(server)
    # Hosting can generate certificates and return to selection cleanly.
    terminal = Terminal(client_dir)
    try:
        terminal.send(b'c'); terminal.send(b'\n\n'); terminal.send(b'h')
        terminal.send(b'\x15' + b'0\n\nn\nn\ny\n')
        terminal.send(b'g'); terminal.send(b'\n'); terminal.send(b'\n')
        output = terminal.read() + terminal.send(b'\n', .5)
        assert os.path.exists(client_dir + '/chatbox/tls/host-key.pem')
        output = terminal.send(b'\x1bOR')
        assert b'chatbox:1|' in output, output
        terminal.send(b'\x1b'); terminal.send(b'\x1b'); terminal.send(b'\n')
        terminal.send(b'q')
        assert terminal.process.wait(timeout=3) == 0
    finally:
        terminal.close()
print('PTY smoke passed: invite join, restart/reconnect, draft retention, 40x12 resize, recent server, TLS generation, hosting summary, return to selection.')
