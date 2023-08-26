param (
    # ローカルPCの秘密鍵のパス
    [Parameter(Mandatory=$true)]
    [string]$privateKey,

    # インスタンス0のアドレス
    [Parameter(Mandatory=$true)]
    [string]$instPublicAddress0,
    [Parameter(Mandatory=$true)]
    [string]$instPrivateAddress0,

    # インスタンス1のアドレス
    [Parameter(Mandatory=$true)]
    [string]$instPublicAddress1,
    [Parameter(Mandatory=$true)]
    [string]$instPrivateAddress1,

    # インスタンス上でキーペアの生成を行う
    [bool]$genKey=$true
)

$ec2UserName = "administrator"

function genAndAppendPublicKey {
    param(
        [string]$clientInstPublicAddress,
        [string]$serverInstPublicAddress,
        [string]$serverInstPrivateAddress,
        [bool]$genKey = $true
    )

    Write-Output "----------------------------------------------------------------"

    if ($genKey) {
        # クライアント側インスタンスでキーペアを作成、ローカルPCに公開鍵をダウンロードする。
        $sshCommand = "ssh-keygen -f `$home/.ssh/id_rsa -t rsa -N '`"`"'"
        # Write-Output ${sshCommand}
        ssh -i ${privateKey} ${ec2UserName}@${clientInstPublicAddress} ${sshCommand}
        scp -i ${privateKey} ${ec2UserName}@${clientInstPublicAddress}:.ssh/id_rsa.pub ./id_rsa.pub

        # 公開鍵の内容をサーバー側インスタンスの認証済鍵リストに追加。
        $publicKey = Get-Content ./id_rsa.pub -encoding ascii -raw
        Remove-Item ./id_rsa.pub
        $sshCommand = "Add-Content `${env:ProgramData}/ssh/administrators_authorized_keys '${publicKey}'"
        # Write-Output $sshCommand
        ssh -i ${privateKey} ${ec2UserName}@${serverInstPublicAddress} ${sshCommand}

        # SSHサーバーをリスタート。
        ssh -i ${privateKey} ${ec2UserName}@${serverInstPublicAddress} 'Restart-Service sshd'
    }

    # クライアント側インスタンスのknown_hostsにサーバーを登録。
    $serverPublicKeyPath = "./ssh_host_ecdsa_key.pub"
    scp -i ${privateKey} ${ec2UserName}@${serverInstPublicAddress}:C:\ProgramData\ssh\ssh_host_ecdsa_key.pub ${serverPublicKeyPath}
    $serverPublicKey = Get-Content -Path ${serverPublicKeyPath} -Raw
    Remove-Item ${serverPublicKeyPath}
    $array = ${serverPublicKey} -split '\s+'
    $knownHost = "${serverInstPrivateAddress} $($array[0]) $($array[1])"
    $sshCommand = "Add-Content `$home/.ssh/known_hosts '${knownHost}'"
    ssh -i ${privateKey} ${ec2UserName}@${clientInstPublicAddress} ${sshCommand}

    # インスタンス間SSH疎通確認。
    $message = "SSH ${clientInstPublicAddress} -> ${serverInstPublicAddress} (private: ${serverInstPrivateAddress})"
    Write-Output $message
    $sshCommand = "ssh -i `$home\.ssh\id_rsa ${ec2UserName}@${serverInstPrivateAddress} 'echo Success!!'"
    ssh -i ${privateKey} ${ec2UserName}@${clientInstPublicAddress} ${sshCommand}
}

genAndAppendPublicKey ${instPublicAddress0} ${instPublicAddress1} ${instPrivateAddress1} ${genKey}
genAndAppendPublicKey ${instPublicAddress1} ${instPublicAddress0} ${instPrivateAddress0} ${genKey}
