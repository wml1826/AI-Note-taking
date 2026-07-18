import subprocess, os, signal
try:
    out = subprocess.check_output(['netstat', '-ano'], stderr=subprocess.DEVNULL).decode()
    for line in out.splitlines():
        if ':8000' in line and 'LISTENING' in line:
            pid = line.strip().split()[-1]
            print('killing pid', pid)
            try:
                os.kill(int(pid), signal.SIGTERM)
            except Exception as e:
                print('kill err', e)
except Exception as e:
    print('err', e)
