#!/usr/bin/env python3
"""Exercise client CLI persistence, secret handling and live quota telemetry."""
import argparse
import http.server
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    binary = str(parser.parse_args().binary.resolve())
    with tempfile.TemporaryDirectory(prefix='literouter-client-cli-') as directory:
        root = Path(directory)
        config_path = root / 'config.json'
        env = dict(os.environ, LITEROUTER_CONFIG=str(config_path), LITEROUTER_STATE_DIR=str(root),
                   LR_CLI_ADMIN='admin-for-cli-test', LR_CLI_CLIENT='client-for-cli-test')
        with socket.socket() as port_socket:
            port_socket.bind(('127.0.0.1', 0))
            port = port_socket.getsockname()[1]
        config_path.write_text(json.dumps({'schema': 1, 'server': {'api_key': '${LR_CLI_ADMIN}',
            'host': '127.0.0.1', 'port': port, 'persist_telemetry': False}, 'providers': [], 'routes': []}))

        def cli(*args, stdin=None, ok=True):
            result = subprocess.run([binary, '--no-color', *args], env=env, input=stdin,
                                    capture_output=True, text=True, timeout=20)
            if ok:
                assert result.returncode == 0, (args, result.stderr)
            else:
                assert result.returncode != 0, args
            for secret in ('admin-for-cli-test', 'client-for-cli-test', 'literal-secret-for-cli-test', 'fallback-secret-for-cli-test'):
                assert secret not in result.stdout + result.stderr, 'secret was printed'
            return result

        def config():
            return json.loads(config_path.read_text())

        cli('clients', '--help')
        cli('--lang', 'zh', 'clients', 'keys', 'add', '--help')
        cli('clients', 'add', 'team', '--name', 'Team', '--rpm', '60', '--concurrent', '2',
            '--requests-per-day', '10', '--tokens-per-day', '10000', '--token-reservation', '100',
            '--model', 'smoke', '--group', 'team')
        cli('clients', 'keys', 'add', 'team', 'desktop', '--key-env', 'LR_CLI_CLIENT')
        cli('clients', 'keys', 'add', 'team', 'server', '--key-stdin', stdin='literal-secret-for-cli-test\n')
        cli('clients', 'keys', 'add', 'team', 'fallback', '--key-stdin',
            stdin='${LR_CLI_FALLBACK:-fallback-secret-for-cli-test}\n')
        cli('clients', 'show', 'team', '--json')
        before = config_path.read_bytes()
        cli('clients', 'keys', 'add', 'team', 'desktop', '--key-env', 'OTHER', ok=False)
        cli('clients', 'keys', 'add', 'team', 'other', '--key-env', 'BAD-NAME', ok=False)
        cli('clients', 'update', 'team', '--tokens-per-day', '20', ok=False)
        cli('clients', 'update', 'team', '--rpm', '-1', ok=False)
        cli('clients', 'update', 'team', '--requests-per-day', '9007199254740992', ok=False)
        cli('clients', 'update', 'team', '--tokens-per-day', '9007199254740992', ok=False)
        cli('clients', 'remove', 'missing', ok=False)
        assert config_path.read_bytes() == before
        cli('clients', 'update', 'team', '--name', 'Renamed')
        client = config()['clients'][0]
        assert client['requests_per_minute'] == 60 and client['keys'][0]['api_key'] == '${LR_CLI_CLIENT}'
        assert client['keys'][2]['api_key'] == '${LR_CLI_FALLBACK:-fallback-secret-for-cli-test}'
        assert client['models'] == ['smoke'] and client['provider_groups'] == ['team']
        cli('clients', 'keys', 'disable', 'team', 'server')
        assert not config()['clients'][0]['keys'][1]['enabled']
        cli('clients', 'keys', 'enable', 'team', 'server')
        cli('clients', 'keys', 'replace', 'team', 'server', '--key-env', 'LR_CLI_OTHER')
        assert config()['clients'][0]['keys'][1]['api_key'] == '${LR_CLI_OTHER}'
        cli('clients', 'keys', 'remove', 'team', 'server')
        cli('clients', 'keys', 'remove', 'team', 'fallback')
        cli('clients', 'disable', 'team')
        assert not config()['clients'][0]['enabled']
        cli('clients', 'enable', 'team')
        cli('clients', 'update', 'team', '--all-models', '--all-groups')
        assert config()['clients'][0]['models'] == [] and config()['clients'][0]['provider_groups'] == []
        cli('clients', 'update', 'team', '--model', 'smoke', '--group', 'team')
        shown = json.loads(cli('clients', 'show', 'team', '--json').stdout)['clients'][0]
        assert shown['keys'] == [{'id': 'desktop', 'enabled': True, 'key_set': True}]
        cli('--lang', 'zh', 'clients', 'show', 'team')

        class Mock(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                self.rfile.read(int(self.headers['Content-Length']))
                body = json.dumps({'id': 'cli-test', 'choices': [{'index': 0,
                    'message': {'role': 'assistant', 'content': 'ok'}, 'finish_reason': 'stop'}],
                    'usage': {'prompt_tokens': 5, 'completion_tokens': 3, 'total_tokens': 8}}).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *_):
                pass

        upstream = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Mock)
        threading.Thread(target=upstream.serve_forever, daemon=True).start()
        cli('providers', 'add', 'mock', '--base-url', f'http://127.0.0.1:{upstream.server_port}/v1',
            '--model', 'smoke', '--group', 'old')
        cli('providers', 'groups', 'mock', '--group', 'team')
        assert config()['providers'][0]['groups'] == ['team']
        server_log = open(root / 'server.log', 'w')
        server = subprocess.Popen([binary, 'serve'], env=env, stdout=server_log, stderr=server_log)
        try:
            for _ in range(100):
                assert server.poll() is None, (root / 'server.log').read_text()
                try:
                    request = urllib.request.Request(f'http://127.0.0.1:{port}/__literouter/status',
                        headers={'Authorization': 'Bearer admin-for-cli-test'})
                    urllib.request.urlopen(request, timeout=.2).read()
                    break
                except (OSError, urllib.error.URLError):
                    time.sleep(.1)
            else:
                raise AssertionError('proxy did not start')
            request = urllib.request.Request(f'http://127.0.0.1:{port}/v1/chat/completions',
                json.dumps({'model': 'smoke', 'messages': [{'role': 'user', 'content': 'test'}]}).encode(),
                {'Content-Type': 'application/json', 'Authorization': 'Bearer client-for-cli-test'})
            assert urllib.request.urlopen(request, timeout=10).status == 200
            usage = json.loads(cli('clients', 'usage', 'team', '--json').stdout)['clients'][0]
            assert usage['requests'] == 1 and usage['successes'] == 1
            assert usage['requests_today'] == 1 and usage['tokens_today'] == 8
            assert usage['tokens_prompt'] == 5 and usage['tokens_completion'] == 3
            cli('--lang', 'zh', 'clients', 'usage')
        finally:
            server.terminate()
            server.wait(timeout=15)
            server_log.close()
            upstream.shutdown()
        cli('providers', 'groups', 'mock', '--clear')
        cli('clients', 'remove', 'team')
        assert config()['clients'] == [] and config()['server']['api_key'] == '${LR_CLI_ADMIN}'
    print('client CLI: persistence, validation, secret redaction and live usage PASS')


if __name__ == '__main__':
    main()
