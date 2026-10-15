[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$VcpkgRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path $PSScriptRoot -Parent
$expectedWechatVersion = '3.9.11.25'
$driverVersion = '39.3.3.1'
$vcpkgRevision = '10b7a178346f3f0abef60cecd5130e295afd8da4'
$utf8 = [System.Text.UTF8Encoding]::new($false)

function Invoke-Checked {
    param([string]$Program, [string[]]$ArgumentList)
    & $Program @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "$Program 退出码为 $LASTEXITCODE"
    }
}

# 在构建入口校验目标版本，防止其他上游分支产物被当成此微信版本的驱动。
$sourceHeader = Get-Content -LiteralPath (Join-Path $repoRoot 'WeChatFerry/spy/spy.h') -Raw
$supportedVersion = [regex]::Match($sourceHeader, '#define SUPPORT_VERSION L"([^"]+)"').Groups[1].Value
if ($supportedVersion -ne $expectedWechatVersion) { throw '源码适配的微信版本与构建目标不一致' }
$sourceCommit = (& git -C $repoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw '无法读取源代码提交' }
$VcpkgRoot = (Resolve-Path -LiteralPath $VcpkgRoot).Path
$actualVcpkgRevision = (& git -C $VcpkgRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $actualVcpkgRevision -ne $vcpkgRevision) { throw 'vcpkg 版本与锁定版本不一致' }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.ComponentGroup.VC.Tools.142.x86.x64 -property installationPath)
if ($LASTEXITCODE -ne 0 -or -not $vsRoot) { throw '缺少 MSVC v142 x64 工具集' }
$compilerRoot = Get-ChildItem -LiteralPath (Join-Path $vsRoot 'VC/Tools/MSVC') -Directory |
    Where-Object Name -Like '14.29.*' | Sort-Object Name -Descending | Select-Object -First 1
if (-not $compilerRoot) { throw '未找到 MSVC 14.29 工具目录' }
$dumpbin = Join-Path $compilerRoot.FullName 'bin/Hostx64/x64/dumpbin.exe'

$env:VCPKG_ROOT = $VcpkgRoot
$env:VCPKG_OVERLAY_TRIPLETS = Join-Path $PSScriptRoot 'triplets'
if (-not $env:PKG_CONFIG -or -not (Test-Path -LiteralPath $env:PKG_CONFIG -PathType Leaf)) {
    throw '请通过 PKG_CONFIG 指定可运行的原生 pkg-config.exe'
}
$pkgConfigVersion = (& $env:PKG_CONFIG --version).Trim()
if ($LASTEXITCODE -ne 0) { throw 'pkg-config 无法运行' }
if ($env:VCPKG_DEFAULT_BINARY_CACHE) {
    New-Item -ItemType Directory -Path $env:VCPKG_DEFAULT_BINARY_CACHE -Force | Out-Null
}
Invoke-Checked (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat') @('-disableMetrics')
Invoke-Checked (Join-Path $VcpkgRoot 'vcpkg.exe') @('integrate', 'install')

Push-Location (Join-Path $repoRoot 'WeChatFerry')
try {
    Invoke-Checked (Join-Path $VcpkgRoot 'vcpkg.exe') @('install', '--triplet', 'x64-windows-static', '--host-triplet', 'x64-windows-static', "--overlay-triplets=$env:VCPKG_OVERLAY_TRIPLETS")
    Invoke-Checked 'msbuild' @('WeChatFerry.sln', '/m', '/p:Configuration=Release', '/p:Platform=x64', '/p:PlatformToolset=v142', '/p:WindowsTargetPlatformVersion=10.0.19041.0', '/p:VcpkgTriplet=x64-windows-static', '/p:VcpkgHostTriplet=x64-windows-static', '/p:VcpkgEnableManifest=true', '/verbosity:minimal')
} finally {
    Pop-Location
}

$outputDirectory = Join-Path $repoRoot "artifacts/WeChatFerry-$driverVersion-WeChat-$expectedWechatVersion-x64"
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$files = @()
foreach ($name in @('sdk.dll', 'spy.dll')) {
    $source = Join-Path $repoRoot "WeChatFerry/Out/$name"
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "缺少构建产物 $name" }
    $bytes = [System.IO.File]::ReadAllBytes($source)
    $peOffset = [BitConverter]::ToInt32($bytes, 60)
    if ([BitConverter]::ToUInt32($bytes, $peOffset) -ne 17744 -or [BitConverter]::ToUInt16($bytes, $peOffset + 4) -ne 34404) {
        throw "$name 不是有效的 x64 PE 文件"
    }
    $exports = (& $dumpbin /exports $source) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "无法检查 $name 导出符号" }
    $requiredExports = if ($name -eq 'spy.dll') { @('InitSpy', 'CleanupSpy') } else { @('WxInitSDK', 'WxDestroySDK') }
    foreach ($export in $requiredExports) {
        if ($exports -notmatch "\b$export\b") { throw "$name 缺少必需导出 $export" }
    }
    if ($name -eq 'spy.dll') {
        $version = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($source)
        if ($version.FileVersion -ne $driverVersion -or $version.ProductVersion -ne $expectedWechatVersion) {
            throw 'spy.dll 版本资源与构建目标不一致'
        }
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $outputDirectory $name) -Force
    $files += [ordered]@{ name = $name; sha256 = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant(); size = $bytes.Length }
}
Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination (Join-Path $outputDirectory 'LICENSE.txt') -Force
$manifest = [ordered]@{
    driverVersion = $driverVersion
    wechatVersion = $expectedWechatVersion
    architecture = 'x64'
    sourceCommit = $sourceCommit
    vcpkgCommit = $vcpkgRevision
    platformToolset = 'v142'
    pkgConfigVersion = $pkgConfigVersion
    files = $files
    liveWechatTestPerformed = $false
}
[System.IO.File]::WriteAllText((Join-Path $outputDirectory 'manifest.json'), ($manifest | ConvertTo-Json -Depth 6) + "`n", $utf8)
Write-Output "驱动已构建并核对：$outputDirectory"
