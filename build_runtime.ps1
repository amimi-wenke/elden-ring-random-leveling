param([string]$Compiler = (Join-Path $PSScriptRoot '.build\w64devkit\bin\g++.exe'))

$ErrorActionPreference = 'Stop'
$output = Join-Path $PSScriptRoot 'runtime\random-leveling.dll'
$source = Join-Path $PSScriptRoot 'runtime\random_leveling.cpp'
$probe = Join-Path $PSScriptRoot 'runtime\probe_memory.S'

& $Compiler -std=c++20 -O2 -Wall -Wextra -Werror -shared -static -s -o "$output.build" $source $probe
if ($LASTEXITCODE -ne 0) { throw "Random Leveling DLL build failed with status $LASTEXITCODE" }
Move-Item -LiteralPath "$output.build" -Destination $output -Force
