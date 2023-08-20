param (
    [string]$CudaInstaller
)

if ($CudaInstaller -ne "" -and !(Test-Path $CudaInstaller)) {
    Write-Error "The file at path $CudaInstaller does not exist."
    exit 1
}

Write-Output "Start to install miscellaneous things."

Invoke-Expression "& {$(Invoke-RestMethod get.scoop.sh)} -RunAsAdmin"
scoop install git
scoop update
scoop bucket add extras

scoop install python@3.11.4

pip install paramiko
pip install numpy
pip install scipy
pip install Pillow

Write-Output "Finished to install miscellaneous things."



Write-Output "Start to install NVIDIA driver"
# ダウンロードされたパスは次のようになっている
# "C:\Users\Administrator\Desktop\NVIDIA\windows\latest\531.79_Cloud_Gaming_win10_win11_server2019_server2022_dch_64bit_international.exe"
$Bucket = "nvidia-gaming"
$KeyPrefix = "windows/latest"
$LocalPath = "$home\Desktop\NVIDIA"
$Objects = Get-S3Object -BucketName $Bucket -KeyPrefix $KeyPrefix -Region us-east-1
foreach ($Object in $Objects) {
    $LocalFileName = $Object.Key
    if ($LocalFileName -ne '' -and $Object.Size -ne 0) {
        $LocalFilePath = Join-Path $LocalPath $LocalFileName
        Copy-S3Object -BucketName $Bucket -Key $Object.Key -LocalFile $LocalFilePath -Region us-east-1
    }
}
$err = (Start-Process -FilePath (Get-ChildItem -Path $home\Desktop\NVIDIA\windows\latest\*.exe -File)[0] -ArgumentList "-s" -Wait -NoNewWindow -PassThru).ExitCode
if ($err -ne 0) {
    Write-Error "Failed to install NVIDIA driver"
}
else {
    New-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Services\nvlddmkm\Global" -Name "vGamingMarketplace" -PropertyType "DWord" -Value "2"
    Invoke-WebRequest -Uri "https://nvidia-gaming.s3.amazonaws.com/GridSwCert-Archive/GridSwCertWindows_2021_10_2.cert" -OutFile "$Env:PUBLIC\Documents\GridSwCert.txt"
    Write-Output "Finished to install NVIDIA driver"
}



$NvSmiDir = "C:\Windows\System32\DriverStore\FileRepository\nvgrid*\"
# $CudaInstaller = "$home\Desktop\cudatoolkit.exe"
if ($CudaInstaller -ne "") {
    Write-Output "Start to install CUDA Toolkit"
    # # ダウンロードに50分ぐらいかかるので本番はS3からダウンロードする。
    # # Invoke-WebRequest -Uri "https://developer.download.nvidia.com/compute/cuda/12.1.1/local_installers/cuda_12.1.1_531.14_windows.exe" -OutFile $CudaInstaller
    # Copy-S3Object -BucketName rtcamp9-test -Key CUDA_Toolkit/cuda_12.1.1_531.14_windows.exe -LocalFile $CudaInstaller -Region ap-northeast-1
    
    # CUDA Toolkitを-s -nオプションのみでインストールすると既にインストールしたNVIDIAドライバーをCUDA Toolkit内のドライバで上書きされるらしい。
    # そうなるとVulkanが使えなくなるようなので必要そうなライブラリのみを指定してインストールする。
    # https://docs.nvidia.com/cuda/cuda-installation-guide-microsoft-windows/index.html
    $err = (Start-Process -FilePath $CudaInstaller -ArgumentList "-s -n cudart_12.1 nvcc_12.1 nvjitlink_12.1 nvrtc_12.1 nvtx_12.1 thrust_12.1 cublas_12.1 cufft_12.1 curand_12.1 cusolver_12.1 cusparse_12.1 npp_12.1 nvjpeg_12.1" -Wait -NoNewWindow -PassThru).ExitCode
    if ($err -ne 0) {
        Write-Error "Failed to install CUDA Toolkit"
    }
    else {
        # CUDA Toolkitからインストールするライブラリを指定すると環境変数が自動的に設定されないのでここで設定する。
        $CudaPath = [Environment]::GetEnvironmentVariable('CUDA_PATH', 'Machine')
        $UserPath = [Environment]::GetEnvironmentVariable('PATH', 'User')
        [Environment]::SetEnvironmentVariable('path', $CudaPath + '\bin;' + $UserPath, 'User')
        Write-Output "Finished to install CUDA Toolkit"

        # CUDA Toolkitをインストールするとnvidia-smiの場所が変わる。
        $NvSmiDir = ".\"
    }
}



# 結果の安定化のためにGPUクロックを固定する。
# https://docs.aws.amazon.com/ja_jp/AWSEC2/latest/UserGuide/optimize_gpu.html
cd $NvSmiDir
$err = (Start-Process -FilePath nvidia-smi -ArgumentList "-ac","5001,1590" -Wait -NoNewWindow -PassThru).ExitCode
if ($err -ne 0) {
    Write-Error "Failed to nvidia-smi"
}
Write-Output "Fixed GPU clock"
