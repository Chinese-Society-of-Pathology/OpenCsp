param(
  [string]$CompilerPath = "D:\mingw64\bin\g++.exe",
  [string]$Source = "DecodeCsp.c",
  [string]$Output = "DecodeCsp.exe",
  [switch]$RunAfterBuild,
  [string]$SampleFile = "sample.csp"
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptDir

if (-not (Test-Path -LiteralPath $CompilerPath)) {
  throw "未找到编译器: $CompilerPath"
}

if (-not (Test-Path -LiteralPath $Source)) {
  throw "未找到源码文件: $Source"
}

$compileArgs = @(
  "-std=c++17"
  "-O2"
  "-Wall"
  "-Wextra"
  "-pedantic"
  $Source
  "-o"
  $Output
)

Write-Host "开始编译..."
Write-Host "  Compiler: $CompilerPath"
Write-Host "  Source:   $Source"
Write-Host "  Output:   $Output"

& $CompilerPath @compileArgs
if ($LASTEXITCODE -ne 0) {
  throw "编译失败，退出码: $LASTEXITCODE"
}

Write-Host "编译成功: $Output"

if ($RunAfterBuild) {
  if (-not (Test-Path -LiteralPath $SampleFile)) {
    throw "未找到示例文件: $SampleFile"
  }

  Write-Host "运行示例: $SampleFile"
  & ".\$Output" $SampleFile

  if ($LASTEXITCODE -ne 0) {
    throw "示例运行失败，退出码: $LASTEXITCODE"
  }
}
