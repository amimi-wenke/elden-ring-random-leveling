param([string]$Compiler = (Join-Path $PSScriptRoot '.build\w64devkit\bin\g++.exe'))

$ErrorActionPreference = 'Stop'
$output = Join-Path $PSScriptRoot 'runtime\one-life.dll'
$source = Join-Path $PSScriptRoot 'runtime\one_life.cpp'
$probe = Join-Path $PSScriptRoot 'runtime\probe_memory.S'

& $Compiler -std=c++20 -O2 -Wall -Wextra -Werror -shared -static -s -o "$output.build" $source $probe
if ($LASTEXITCODE -ne 0) { throw "One Life DLL build failed with status $LASTEXITCODE" }
Move-Item -LiteralPath "$output.build" -Destination $output -Force
