import sys
import os
import shutil
from pathlib import Path
import glob
import time
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

def run_remote_command(ssh, cmd, pr=True):
    _, r_stdout, r_stderr = ssh.exec_command(cmd)
    r_status = r_stdout.channel.recv_exit_status()
    r_stdout = r_stdout.read().decode()
    r_stderr = r_stderr.read().decode()
    if pr:
        print(f"Command exited with status: {r_status}")
        if r_stdout:
            print("Output:", r_stdout)
        if r_stderr:
            print("Errors:", r_stderr)
    return r_status, r_stdout, r_stderr

def run():
    key_path = R'C:\Users\Administrator\.ssh\id_rsa'

    submission_dir = Path(sys.argv[1])
    result_dir = Path(sys.argv[2])
    primary_priv_ip = sys.argv[3]
    secondary_priv_ip = sys.argv[4]
    copy_zip = bool(int(sys.argv[5]))
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
    _, r_stdout, _ = run_remote_command(ssh, f'echo $env:home')
    r_home_dir = r_stdout.rstrip()

    if copy_zip:
        # セカンダリーインスタンスのzip置き場を作成。
        run_remote_command(ssh, 'mkdir $home\\Desktop\\submissions')

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
            root_dir_name = Path(ext_files[0]).parts[0]
            zip_ref.extractall(dir_to_place)

        working_dir = dir_to_place / root_dir_name
        old_dir = chdir(working_dir)

        dst_dir = result_dir / entry.stem
        dst_dir.mkdir(exist_ok=True)
        dst_local_others_dir = dst_dir / 'local_others'
        dst_local_others_dir.mkdir(exist_ok=True)
        dst_remote_others_dir = dst_dir / 'remote_others'
        dst_remote_others_dir.mkdir(exist_ok=True)
        img_dir = result_dir / ('images_' + entry.stem)
        img_dir.mkdir(exist_ok=True)

        # リモートマシンでzipをホームディレクトリに展開。
        r_cmd = 'Expand-Archive -Path {}\\Desktop\\submissions\\{} -DestinationPath {}'.format(r_home_dir, entry.name, r_home_dir)
        run_remote_command(ssh, r_cmd)

        # スライドのコピー。
        deck_files = []
        deck_files += working_dir.glob('*.pdf')
        deck_files += working_dir.glob('*.pptx')
        for deck_file in deck_files:
            shutil.copy2(deck_file, dst_dir)

        def get_local_file_list(dir):
            return set([file.name for file in dir.iterdir() if file.is_file()])

        def get_remote_file_list(dir):
            r_cmd = f'Get-ChildItem -Path {dir} -File | Select-Object -ExpandProperty Name'
            _, r_stdout, _ = run_remote_command(ssh, r_cmd)
            return set(r_stdout.splitlines())

        # レンダリング実行前のファイルリストを生成。
        org_files = []
        for file in ext_files:
            if not file.endswith('/') and len(Path(file).parts) == 2:
                org_files.append(Path(file).name)
        org_files = set(org_files)

        # requirements.txtに従ってパッケージインストール。
        if 'requirements.txt' in org_files:
            cmd = ['pip', 'install', '-r', 'requirements.txt']
            run_command(cmd)
            run_remote_command(ssh, f'pip install -r $home\\{root_dir_name}\\requirements.txt')

        # リモートマシンの現在時刻を取得。
        _, r_stdout, _ = run_remote_command(ssh, '$currentTime = Get-Date;$epoch = Get-Date -Year 1970 -Month 1 -Day 1 -Hour 0 -Minute 0 -Second 0 -Millisecond 0;$unixTimestamp = [Math]::Round(($currentTime - $epoch).TotalSeconds);$unixTimestamp')
        r_start_time = int(r_stdout)

        # ローカルマシンの現在時刻を取得。
        start_time = time.time()

        # レンダリング実行。
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

        # 標準出力とエラー出力を書き出す。
        if stdout:
            with open(dst_dir / 'stdout.log', 'w') as f:
                f.write(stdout)
        if stderr:
            with open(dst_dir / 'stderr.log', 'w') as f:
                f.write(stderr)

        misc_msg = ''

        # fpsを読み取る。
        if (working_dir / 'fps.txt').exists():
            with open('fps.txt', 'r') as f:
                fps = int(f.read())
        else:
            misc_msg += 'fps.txt: not found\n'
            fps = 60

        # ローカルマシンのレンダリング実行後のファイルリストを取得。
        files = get_local_file_list(working_dir)
        new_files = files - org_files

        # リモートマシンのレンダリング実行後のファイルリストを取得。
        r_files = get_remote_file_list(f'$home\\{root_dir_name}')
        r_new_files = r_files - org_files

        # ローカルマシンで出力された画像をコピー。
        for file in new_files:
            src_file = working_dir / file
            if img_regex.match(file):
                if (src_file.stat().st_mtime - start_time) < 310:
                    shutil.copy2(src_file, img_dir)
            else:
                shutil.copy2(src_file, dst_local_others_dir)

        # リモートマシンで出力された画像をコピー。
        sftp = ssh.open_sftp()
        for file in r_new_files:
            remote_path = '{}\\{}\\{}'.format(r_home_dir, root_dir_name, file)
            if img_regex.match(file):
                remote_stat = sftp.stat(remote_path)
                if (remote_stat.st_mtime - start_time) < 310:
                    local_path = '{}\\{}'.format(img_dir, file)
                    sftp.get(remote_path, local_path)
                    os.utime(local_path, (remote_stat.st_atime, remote_stat.st_mtime))
            else:
                sftp.get(remote_path, '{}\\{}'.format(dst_remote_others_dir, file))
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
            old_name = Path(file)
            new_name = old_name.with_name(f'input_{idx:03}{ext}')
            if new_name.exists():
                new_name.unlink()
            old_name.rename(new_name)

        try:
            if len(img_list) > 0:
                # 動画作成。
                cmd = ['ffmpeg']
                # Opitions for input
                cmd += ['-framerate', str(fps), '-i', f'{img_dir}\\input_%03d{ext}']
                # Options for output
                cmd += ['-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-crf', '18', f'{dst_dir}\\result.mp4']
                run_command(cmd)
        except Exception as e:
            misc_msg += 'ffmpeg error.\n'

        if len(img_list) == 0:
            misc_msg += 'No images.\n'
        
        with open(dst_dir / 'misc.txt', 'w') as f:
            f.write(misc_msg)

        chdir(old_dir)

        # リモートマシンで展開したディレクトリを削除。
        r_cmd = f'Remove-Item -Recurse $home\\{root_dir_name}'
        run_remote_command(ssh, r_cmd)

        ssh.close()

        shutil.rmtree(working_dir)

    return 0

if __name__ == '__main__':
    try:
        run()
    except Exception as e:
        print(e)
