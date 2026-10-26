#!/usr/bin/env python3
"""Bounded CLI regression checks with isolated config/state and ephemeral ports."""
import argparse
import hashlib
import http.client
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', required=True, type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    with tempfile.TemporaryDirectory(prefix='literouter-cli-smoke-') as temporary:
        root = Path(temporary)
        config = root / 'fresh' / 'config.json'
        env = os.environ.copy() | {'LITEROUTER_CONFIG': str(config),
            'LITEROUTER_STATE_DIR': str(root / 'state'), 'LITEROUTER_LANG': 'en'}
        for key in ['LITEROUTER_WEB_DIR', 'LITEROUTER_PROXY', 'HTTPS_PROXY', 'https_proxy',
                    'HTTP_PROXY', 'http_proxy', 'NO_PROXY', 'no_proxy']:
            env.pop(key, None)

        def run(*arguments, input=None, code=0):
            reply = subprocess.run([str(binary), *arguments], env=env, input=input,
                                   text=True, capture_output=True, timeout=20)
            assert reply.returncode == code, (arguments, reply.returncode, reply.stderr)
            return reply.stdout

        print('version:', run('--version').strip())
        run('serve', '--check', '--port', '0')
        run('serve', '--print-config', '--port', '0')
        assert not config.exists(), 'read-only serve checks wrote a config'
        process = None
        with (root / 'serve.log').open('wb') as log:
            try:
                process = subprocess.Popen([str(binary), 'serve', '--port', '0', '--quiet', '--no-persist'],
                                           env=env, stdout=log, stderr=log)
                port = None
                for _ in range(120):
                    assert process.poll() is None, (root / 'serve.log').read_text()
                    records = list((root / 'state').glob('literouter-*.pid'))
                    if records:
                        port = json.loads(records[0].read_text())['port']
                        break
                    time.sleep(0.05)
                assert port and port != 8787, 'ephemeral listener never started'
                assert config.is_file(), 'first serve did not create its configuration'
                seed = json.loads(config.read_text())
                assert seed['server']['port'] == 8787, 'one-off CLI override was persisted'
                # Save port 0 in this isolated file to exercise management discovery.
                seed['server']['port'] = 0
                config.write_text(json.dumps(seed))
                snapshot = json.loads(run('status', '--json'))
                assert snapshot['reachable'] and snapshot['port'] == port, snapshot
                assert snapshot['config_writable'], 'new configuration is incorrectly read-only'
                run('logs', '--json')
                run('explain', 'gpt-4o', '--json')
                before = hashlib.sha256(config.read_bytes()).hexdigest()
                parsed = json.loads(run('config', 'load', '-', '--dry-run', '--json',
                    input='{"schema":3,"providers":[],"routes":[]}'))
                assert parsed['ok'] and hashlib.sha256(config.read_bytes()).hexdigest() == before
                run('providers', 'add', 'stdin-check', '--base-url', 'http://127.0.0.1:9',
                    '--key-stdin', input='fake-only-key\r\n')
                providers = json.loads(config.read_text())['providers']
                assert next(item['api_key'] for item in providers if item['id'] == 'stdin-check') == 'fake-only-key'
                connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
                connection.request('POST', '/__literouter/shutdown')
                response = connection.getresponse()
                assert response.status == 200
                response.read()
                connection.close()
                assert process.wait(timeout=10) == 0
                run('status', '--json', code=4)
                # A corrupt file is never overwritten by startup.
                config.write_text('{broken')
                run('serve', '--port', '0', code=3)
                assert config.read_text() == '{broken'
                print('CLI first-run, dynamic-port, stdin, key preservation and shutdown: PASS')
            finally:
                if process is not None and process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)


if __name__ == '__main__':
    main()
