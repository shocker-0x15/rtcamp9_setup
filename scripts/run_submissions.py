import sys
import os
import shutil
from pathlib import Path
import glob
import re
import subprocess
import zipfile
import paramiko

def chdir(dst):
    oldDir = os.getcwd()
    os.chdir(dst)
    return oldDir

def run_command(cmd, timeout=None):
    print(' '.join(cmd))
    return subprocess.run(cmd, check=True, timeout=timeout, capture_output=True, text=True)

def run():
    key_path = R'C:\Users\Administrator\.ssh\id_rsa'

    submission_dir = Path(sys.argv[1])
    result_dir = Path(sys.argv[2])
    primary_priv_ip = sys.argv[3]
    secondary_priv_ip = sys.argv[4]
    dir_to_place = Path.home()

    if not submission_dir.exists():
        print('Submission directory does not exist.')
        return -1

    result_dir.mkdir(parents=True, exist_ok=True)

    img_regex = re.compile(r'\d\d\d\.(png|jpg|jpeg)')

    ssh = paramiko.SSHClient()
    key = paramiko.RSAKey(filename=key_path)
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(secondary_priv_ip, username='administrator', pkey=key)

    # セカンダリーインスタンスのホームパスを取得。
    r_cmd = f'echo $env:home'
    _, r_stdout, r_stderr = ssh.exec_command(r_cmd)
    exit_status = r_stdout.channel.recv_exit_status()
    r_home_dir = r_stdout.read().decode().rstrip()
    print(f"Command exited with status: {exit_status}")
    print("Output:", r_home_dir)
    print("Errors:", r_stderr.read().decode())

    # セカンダリーインスタンスのzip置き場を作成。
    r_cmd = 'mkdir $home\\Desktop\\submissions'
    _, r_stdout, r_stderr = ssh.exec_command(r_cmd)
    exit_status = r_stdout.channel.recv_exit_status()
    print(f"Command exited with status: {exit_status}")
    print("Output:", r_stdout.read().decode())
    print("Errors:", r_stderr.read().decode())

    # セカンダリーインスタンスにzipコピー。
    sftp = ssh.open_sftp()
    for entry in submission_dir.iterdir():
        if not entry.is_file():
            continue
        sftp.put(entry,
                 '{}\\Desktop\\submissions\\{}'.format(r_home_dir, entry.name))
    sftp.close()

    ssh.close()



    errors = {}
    for entry in submission_dir.iterdir():
        if not entry.is_file():
            continue

        ssh = paramiko.SSHClient()
        key = paramiko.RSAKey(filename=key_path)
        ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        ssh.connect(secondary_priv_ip, username='administrator', pkey=key)

        # zipをホームディレクトリに展開。
        with zipfile.ZipFile(entry, 'r') as zip_ref:
            ext_files = zip_ref.namelist()

            num_dirs = 0
            num_files = 0
            for ext_file in ext_files:
                if len(Path(ext_file).parts) != 1:
                    continue
                if Path(ext_file).is_file():
                    num_files += 1
                else:
                    num_dirs += 1

            if num_dirs == 1 and num_files == 0:
                root_dir_name = ext_files[0]
                zip_ref.extractall(dir_to_place)
            else:
                errors[str(entry)] = 'Failed in zip extraction.'
                continue

        working_dir = dir_to_place / root_dir_name
        old_dir = chdir(working_dir)

        dst_dir = result_dir / working_dir.name
        dst_dir.mkdir(exist_ok=True)
        img_dir = result_dir / ('images_' + working_dir.name)
        img_dir.mkdir(exist_ok=True)

        # リモートPCでzipをホームディレクトリに展開。
        r_cmd = 'Expand-Archive -Path {}\\Desktop\\submissions\\{} -DestinationPath {}'.format(r_home_dir, entry.name, r_home_dir)
        _, r_stdout, r_stderr = ssh.exec_command(r_cmd)
        exit_status = r_stdout.channel.recv_exit_status()
        print(f"Command exited with status: {exit_status}")
        print("Output:", r_stdout.read().decode())
        print("Errors:", r_stderr.read().decode())

        # スライドのコピー。
        deck_files = []
        deck_files += working_dir.glob('*.pdf')
        deck_files += working_dir.glob('*.pptx')
        for deck_file in deck_files:
            shutil.copy2(deck_file, dst_dir)

        # レンダリング実行。
        exe_success = True
        cmd = ['powershell', '-File', './run.ps1']
        # cmd = ['./run.sh']
        cmd += [primary_priv_ip, secondary_priv_ip]
        try:
            output = run_command(cmd, timeout=310)
            stdout = output.stdout
            stderr = output.stderr
        except subprocess.CalledProcessError as e:
            stdout = e.stdout
            stderr = e.stderr
        except Exception as e:
            if e.stdout:
                stdout = e.stdout
            if e.stderr:
                stderr = e.stderr
            exe_success = False

        # 標準出力とエラー出力を書き出す。
        if stdout:
            with open(dst_dir / 'stdout.log', 'w') as f:
                f.write(stdout)
        if stderr:
            with open(dst_dir / 'stderr.log', 'w') as f:
                f.write(stderr)

        if exe_success:
            # fpsを読み取る。
            with open('fps.txt', 'r') as f:
                fps = int(f.read())

            # ローカルマシンで出力された画像をコピー。
            for file in working_dir.iterdir():
                if img_regex.match(file.name):
                    shutil.copy2(file, img_dir)

            r_cmd = f'Get-ChildItem -Path $home\\{root_dir_name} | Select-Object -ExpandProperty Name'
            _, r_stdout, _ = ssh.exec_command(r_cmd)
            r_files = r_stdout.read().decode().splitlines()

            # リモートマシンで出力された画像をコピー。
            sftp = ssh.open_sftp()
            for file in r_files:
                if img_regex.match(file):
                    sftp.get('{}\\{}\\{}'.format(r_home_dir, root_dir_name, file),
                             '{}\\{}'.format(img_dir, file))
            sftp.close()

            # 連番画像における欠落事故防止のため名前でソートしたのち改めて連番化する。
            ext = None
            img_list = []
            for file in img_dir.iterdir():
                if file.is_file() and ext is None:
                    ext = file.suffix
                img_list.append(str(file))
            img_list.sort()
            for idx, file in enumerate(img_list):
                Path(file).rename(Path(file).with_name(f'input_{idx:03}{ext}'))

            # 動画作成。
            cmd = ['ffmpeg', '-framerate', str(fps)]
            cmd += ['-i', f'{img_dir}\\input_%03d{ext}', f'{dst_dir}\\result.mp4']
            run_command(cmd)

        chdir(old_dir)

        # リモートPCで展開したディレクトリを削除。
        r_cmd = f'Remove-Item -Recurse $home\\{root_dir_name}'
        _, r_stdout, r_stderr = ssh.exec_command(r_cmd)
        exit_status = r_stdout.channel.recv_exit_status()
        print(f"Command exited with status: {exit_status}")
        print("Output:", r_stdout.read().decode())
        print("Errors:", r_stderr.read().decode())

        ssh.close()

        shutil.rmtree(working_dir)

    return 0

if __name__ == '__main__':
    try:
        run()
    except Exception as e:
        print(e)