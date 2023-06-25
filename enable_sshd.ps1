Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0

# 一旦起動して初期設定ファイルを生成させる。
Start-Service sshd

# 自動起動設定有効化。
Set-Service -Name sshd -StartupType 'Automatic'

# 明示的に公開鍵認証の有効化とパスワード認証の無効化を行う。
$sshdConfigPath = Join-Path $env:ProgramData 'ssh\sshd_config'
Get-Content $sshdConfigPath -Encoding Ascii -Raw | 
    ForEach-Object {
        $c = $_ -replace '#PubkeyAuthentication', 'PubkeyAuthentication'
        $c = $c -replace '#PasswordAuthentication yes', 'PasswordAuthentication no'
        $c | Out-File -FilePath $sshdConfigPath -Encoding ascii
    }

# サービス再起動。
Restart-Service sshd

# インスタンスメタデータから公開鍵を取得し administrators_authorized_keys に設定。
$administratorsKeyPath = Join-Path $env:ProgramData 'ssh\administrators_authorized_keys'
Invoke-RestMethod -Uri http://169.254.169.254/latest/meta-data/public-keys/0/openssh-key/ |
    Out-File -FilePath $administratorsKeyPath -Encoding ascii

# ファイルのアクセスアクセス権の継承を解除。
$acl = Get-Acl $administratorsKeyPath
$acl.SetAccessRuleProtection($true, $true)
$acl | Set-Acl -Path $administratorsKeyPath

# ファイルの余計なアクセス権を削除。
$acl = Get-Acl $administratorsKeyPath
$ruleToRemove = $acl.Access |
    Where-Object { $_.IdentityReference -eq 'NT AUTHORITY\Authenticated Users' }
$acl.RemoveAccessRule($ruleToRemove)
$acl | Set-Acl -Path $administratorsKeyPath

# デフォルトシェルをPowershellに変更。
New-ItemProperty -Path "HKLM:\SOFTWARE\OpenSSH" -Name DefaultShell -Value "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe" -PropertyType String -Force

# 異なるインスタンス上のアプリケーション間で直接通信する際のポートを許可しておく。
New-NetFirewallRule -DisplayName 'RTCamp9 App to App' -Profile 'Any' -Direction 'Inbound' -Action 'Allow' -Protocol 'TCP' -LocalPort 12345
